#include "symlink.hpp"
#include "desktop_item.hpp"
#include "gobject_ptr.hpp"

#include <string>
#include <vector>
#include <algorithm>

namespace
{
    using Paths = std::vector<std::string>;
    constexpr auto paths_key = "dicons::link-targets";

    struct Watch
    {
        Paths paths;
        std::vector<ObjectPtr<GFileMonitor>> monitors;
        std::function<void()> refresh;

        ~Watch()
        {
            for (const auto &monitor : monitors)
            {
                g_signal_handlers_disconnect_by_data(monitor.get(), this);
                g_file_monitor_cancel(monitor.get());
            }
        }
    };

    void changed(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent, Watch *watch)
    {
        auto *target = G_FILE(g_object_get_data(G_OBJECT(monitor), "link-target"));
        if (g_file_equal(file, target) || (other && g_file_equal(other, target)))
            watch->refresh();
    }
}

// Read each hop off the UI thread
void read_symlink_target(GFileInfo *info, GFile *file)
{
    const char *target = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
    if (!target)
        return;
    Paths paths;
    auto current = retain(file);
    ObjectPtr<GFileInfo> hop;
    while (target && paths.size() < 40)
    {
        ObjectPtr<GFile> parent{g_file_get_parent(current.get())};
        if (!parent)
            break;
        current.reset(g_path_is_absolute(target) ? g_file_new_for_path(target) : g_file_resolve_relative_path(parent.get(), target));
        auto *path = g_file_peek_path(current.get());
        if (!path || std::ranges::find(paths, path) != paths.end())
            break;
        paths.emplace_back(path);
        hop.reset(g_file_query_info(current.get(), "standard::name,standard::type,standard::symlink-target,standard::icon,standard::content-type",
                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr));
        if (!hop)
            break;
        target = g_file_info_get_attribute_byte_string(hop.get(), G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
        if (!target)
        {
            read_launcher_presentation(hop.get(), current.get());
            if (auto *icon = g_file_info_get_icon(hop.get()))
                g_file_info_set_icon(info, icon);
            if (auto *type = g_file_info_get_content_type(hop.get()))
                g_file_info_set_content_type(info, type);
        }
    }
    std::vector<char *> values;
    for (auto &path : paths)
        values.push_back(path.data());
    values.push_back(nullptr);
    g_file_info_set_attribute_stringv(info, paths_key, values.data());
}

const char *symlink_target_path(GFileInfo *info)
{
    auto **paths = g_file_info_get_attribute_stringv(info, paths_key);
    return paths && *paths ? paths[g_strv_length(paths) - 1] : nullptr;
}

void watch_symlink_target(GFileInfo *item, std::function<void()> refresh)
{
    Paths paths;
    for (auto **path = g_file_info_get_attribute_stringv(item, paths_key); path && *path; ++path)
        paths.emplace_back(*path);
    auto *old = static_cast<Watch *>(g_object_get_data(G_OBJECT(item), "link-watch"));
    if (old && old->paths == paths)
        return;
    std::unique_ptr<Watch> watch;
    if (!paths.empty())
    {
        watch = std::make_unique<Watch>();
        watch->paths = std::move(paths);
        watch->refresh = std::move(refresh);
        for (const auto &path : watch->paths)
        {
            ObjectPtr<GFile> file{g_file_new_for_path(path.c_str())};
            ObjectPtr<GFileMonitor> monitor{g_file_monitor_file(file.get(), G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr)};
            if (!monitor)
                continue;
            g_object_set_data_full(G_OBJECT(monitor.get()), "link-target", file.release(), g_object_unref);
            g_signal_connect(monitor.get(), "changed", G_CALLBACK(changed), watch.get());
            watch->monitors.push_back(std::move(monitor));
        }
    }
    g_object_set_data_full(G_OBJECT(item), "link-watch", watch.release(), [](gpointer p)
                           { delete static_cast<Watch *>(p); });
}
