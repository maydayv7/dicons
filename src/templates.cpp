#include "templates.hpp"
#include "desktop_directory.hpp"

#include <algorithm>
#include <vector>

static ObjectPtr<GtkDirectoryList> watch_templates()
{
    const char *path = g_get_user_special_dir(G_USER_DIRECTORY_TEMPLATES);
    g_autofree char *fallback = g_build_filename(g_get_home_dir(), "Templates", nullptr);
    ObjectPtr<GFile> folder{g_file_new_for_path(path ? path : fallback)};
    ObjectPtr<GFile> home{g_file_new_for_path(g_get_home_dir())};
    if (g_file_equal(folder.get(), home.get()))
        return nullptr;
    return ObjectPtr<GtkDirectoryList>{gtk_directory_list_new("standard::name,standard::display-name,standard::type,standard::is-hidden,standard::is-symlink", folder.get())};
}

static void refresh_templates(GListModel *model, guint, guint, guint, GMenu *section)
{
    g_menu_remove_all(section);
    std::vector<ObjectPtr<GFileInfo>> files;
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(model, i))};
        if (!g_file_info_get_is_hidden(info.get()) && !g_file_info_get_is_symlink(info.get()) &&
            g_file_info_get_file_type(info.get()) == G_FILE_TYPE_REGULAR)
            files.push_back(std::move(info));
    }
    std::ranges::sort(files, [](const auto &a, const auto &b)
                      { return g_utf8_collate(g_file_info_get_display_name(a.get()), g_file_info_get_display_name(b.get())) < 0; });
    for (const auto &info : files)
    {
        g_autofree char *uri = g_file_get_uri(item_file(info.get()));
        // GtkPopoverMenu treats underscores as mnemonic markers
        g_auto(GStrv) parts = g_strsplit(g_file_info_get_display_name(info.get()), "_", -1);
        g_autofree char *label = g_strjoinv("__", parts);
        g_autoptr(GMenuItem) item = g_menu_item_new(label, nullptr);
        g_menu_item_set_action_and_target(item, "desktop.template", "s", uri);
        g_menu_append_item(section, item);
    }
}

void append_templates(GMenu *menu)
{
    auto templates = watch_templates();
    if (!templates)
        return;

    // Enumerate on each opening
    g_autoptr(GMenu) section = g_menu_new();
    g_signal_connect_object(templates.get(), "items-changed", G_CALLBACK(refresh_templates), section, GConnectFlags{});
    refresh_templates(G_LIST_MODEL(templates.get()), 0, 0, 0, section);
    g_object_set_data_full(G_OBJECT(section), "templates", templates.release(), g_object_unref);
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
}
