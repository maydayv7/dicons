#include "desktop_directory.hpp"
#include "desktop_item.hpp"
#include "file_rename.hpp"
#include "notifications.hpp"
#include "symlink.hpp"
#include "grid_layout.hpp"
#include "thumbnail_cache.hpp"

#include <glib/gstdio.h>
#include <cerrno>
#include <deque>
#include <filesystem>
#include <unistd.h>

ObjectPtr<GFile> open_desktop_folder(GSettings *settings, GError **error)
{
    g_autofree char *configured = g_settings_get_string(settings, "directory");
    const bool use_xdg = *configured == '\0';
    const char *path = use_xdg ? g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP) : configured;
    if (!path)
    {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "No XDG Desktop directory is configured.");
        return nullptr;
    }
    if (!g_path_is_absolute(path))
    {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "The directory setting must be an absolute local path: %s", path);
        return nullptr;
    }

    ObjectPtr<GFile> folder{g_file_new_for_path(path)};
    path = g_file_peek_path(folder.get());
    if (use_xdg)
    {
        std::error_code ec;
        if (g_str_equal(path, g_get_home_dir()) || std::filesystem::equivalent(path, g_get_home_dir(), ec))
            return nullptr;
        if (g_mkdir_with_parents(path, 0777) == -1)
        {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create desktop directory %s: %s", path, g_strerror(errno));
            return nullptr;
        }
    }

    g_autoptr(GDir) dir = g_dir_open(path, 0, error);
    if (!dir)
        return nullptr;
    if (g_access(path, X_OK) == -1)
    {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot access desktop directory %s: %s", path, g_strerror(errno));
        return nullptr;
    }
    return folder;
}

GFile *item_file(GFileInfo *info)
{
    return G_FILE(g_file_info_get_attribute_object(info, "standard::file"));
}

FileSet make_file_set()
{
    auto equal = [](gconstpointer a, gconstpointer b)
    { return g_file_equal(G_FILE(a), G_FILE(b)); };
    return FileSet{g_hash_table_new_full(g_file_hash, equal, g_object_unref, nullptr)};
}

struct Visibility
{
    FileSet staging;
    ObjectPtr<GSimpleAction> show_hidden;
};

static gboolean visible_item(gpointer item, gpointer data)
{
    auto &visibility = *static_cast<Visibility *>(data);
    auto *info = G_FILE_INFO(item);
    if (g_hash_table_contains(visibility.staging.get(), item_file(info)))
        return false;
    g_autoptr(GVariant) show = g_action_get_state(G_ACTION(visibility.show_hidden.get()));
    return g_variant_get_boolean(show) || !g_file_info_get_is_hidden(info);
}

static void directory_error(GtkDirectoryList *list, GParamSpec *, GtkApplication *app)
{
    if (const auto *error = gtk_directory_list_get_error(list))
        notify_error(G_APPLICATION(app), "Could not read the Desktop folder", error->message);
}

// GtkDirectoryList enumerates the initial contents, our monitor reconciles changed paths
class DesktopDirectory::Updates : public std::enable_shared_from_this<Updates>
{
public:
    ObjectPtr<GListStore> entries{g_list_store_new(G_TYPE_FILE_INFO)};

    void start(const DesktopDirectory &directory, GFileMonitor *monitor, GtkApplication *app, GtkFilter *filter)
    {
        folder_ = retain(directory.folder.get());
        settings_ = retain(directory.preferences.get());
        filter_ = retain(filter);
        sorter_ = retain(GTK_SORTER(directory.sorting.sorter.get()));
        item_updates_ = retain(directory.item_updates.get());
        if (monitor)
        {
            monitor_ = retain(monitor);
            g_signal_connect(monitor, "changed", G_CALLBACK(directory_changed), this);
        }
        scan_.reset(gtk_directory_list_new(attributes, nullptr));
        gtk_directory_list_set_monitored(scan_.get(), false);
        g_signal_connect(scan_.get(), "items-changed", G_CALLBACK(scanned_items), this);
        g_signal_connect(scan_.get(), "notify::loading", G_CALLBACK(scan_progress), this);
        g_signal_connect_object(scan_.get(), "notify::error", G_CALLBACK(directory_error), app, GConnectFlags{});
        gtk_directory_list_set_file(scan_.get(), folder_.get());
    }

    ~Updates()
    {
        if (monitor_)
            g_signal_handlers_disconnect_by_data(monitor_.get(), this);
        if (scan_)
            g_signal_handlers_disconnect_by_data(scan_.get(), this);
    }

private:
    static constexpr auto attributes = "standard::name,standard::display-name,standard::edit-name,standard::icon,"
                                       "standard::type,standard::is-hidden,standard::is-symlink,standard::symlink-target,"
                                       "standard::content-type,standard::size,access::can-read,"
                                       "time::modified,time::modified-nsec,id::file,time::created,time::created-nsec";
    ObjectPtr<GtkDirectoryList> scan_;
    ObjectPtr<GFile> folder_;
    ObjectPtr<GSettings> settings_;
    ObjectPtr<GtkFilter> filter_;
    ObjectPtr<GtkSorter> sorter_;
    ObjectPtr<DiconsItemUpdates> item_updates_;
    ObjectPtr<GFileMonitor> monitor_;
    std::deque<ObjectPtr<GFile>> pending_;
    FileSet queued_ = make_file_set();
    bool reading_{};

    static void scanned_items(GListModel *scan, guint position, guint removed, guint added, Updates *self)
    {
        g_autoptr(GPtrArray) batch = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = position; i < position + added; ++i)
        {
            auto *info = G_FILE_INFO(g_list_model_get_item(scan, i));
            cache_filename_key(info);
            if (is_desktop_launcher(info) || g_file_info_get_is_symlink(info))
                self->queue(item_file(info));
            g_ptr_array_add(batch, info);
        }
        g_list_store_splice(self->entries.get(), position, removed, batch->pdata, batch->len);
    }

    static void scan_progress(GtkDirectoryList *scan, GParamSpec *, Updates *self)
    {
        if (gtk_directory_list_is_loading(scan))
            return;
        g_signal_handlers_disconnect_by_data(scan, self);
        self->scan_.reset();
        // Subsequent monitor events can locate a row by basename
        g_list_store_sort(self->entries.get(), [](gconstpointer a, gconstpointer b, gpointer)
                          { return g_strcmp0(g_file_info_get_name(G_FILE_INFO(a)), g_file_info_get_name(G_FILE_INFO(b))); }, nullptr);
        self->read_next();
    }

    static void directory_changed(GFileMonitor *, GFile *file, GFile *other,
                                  GFileMonitorEvent event, Updates *self)
    {
        switch (event)
        {
        case G_FILE_MONITOR_EVENT_RENAMED:
            relocate_position(self->settings_.get(), file, other);
            self->queue(file);
            self->queue(other);
            break;
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_DELETED:
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_CHANGED:
        case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_MOVED_OUT:
            self->queue(file);
            break;
        default:
            return;
        }
        self->read_next();
    }

    void queue(GFile *file)
    {
        if (file && g_file_has_parent(file, folder_.get()) && g_hash_table_add(queued_.get(), g_object_ref(file)))
            pending_.push_back(retain(file));
    }

    void read_next()
    {
        if (scan_ || reading_ || pending_.empty())
            return;
        auto file = std::move(pending_.front());
        pending_.pop_front();
        g_hash_table_remove(queued_.get(), file.get());
        reading_ = true;
        ObjectPtr<GTask> task{g_task_new(file.get(), nullptr, file_read, new std::weak_ptr<Updates>(weak_from_this()))};
        g_task_run_in_thread(task.get(), read_info);
    }

    static void read_info(GTask *task, gpointer source, gpointer, GCancellable *)
    {
        auto *file = G_FILE(source);
        g_autoptr(GError) error = nullptr;
        auto *info = g_file_query_info(file, attributes, G_FILE_QUERY_INFO_NONE, nullptr, &error);
        if (!info)
        {
            g_task_return_error(task, g_steal_pointer(&error));
            return;
        }
        cache_link_identity(info, file);
        read_symlink_target(info, file);
        read_launcher_presentation(info, file);
        cache_filename_key(info);
        g_file_info_set_attribute_object(info, "standard::file", G_OBJECT(file));
        g_task_return_pointer(task, info, g_object_unref);
    }

    static void file_read(GObject *source, GAsyncResult *result, gpointer data)
    {
        std::unique_ptr<std::weak_ptr<Updates>> lifetime{static_cast<std::weak_ptr<Updates> *>(data)};
        g_autoptr(GError) error = nullptr;
        auto *file = G_FILE(source);
        ObjectPtr<GFileInfo> info{static_cast<GFileInfo *>(g_task_propagate_pointer(G_TASK(result), &error))};
        if (auto self = lifetime->lock())
        {
            // Newer event supersedes in-flight lookup
            if (!g_hash_table_contains(self->queued_.get(), file) &&
                (info || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)))
                self->apply(file, info.get());
            self->reading_ = false;
            self->read_next();
        }
    }

    void apply(GFile *file, GFileInfo *info)
    {
        auto *model = G_LIST_MODEL(entries.get());
        g_autofree char *name = g_file_get_basename(file);
        guint first = 0;
        guint last = g_list_model_get_n_items(model);
        while (first < last)
        {
            const guint i = first + (last - first) / 2;
            ObjectPtr<GFileInfo> current{G_FILE_INFO(g_list_model_get_item(model, i))};
            const int cmp = g_strcmp0(g_file_info_get_name(current.get()), name);
            if (cmp < 0)
                first = i + 1;
            else if (cmp > 0)
                last = i;
            else
            {
                if (info)
                {
                    update_item(i, current.get(), info);
                }
                else
                {
                    relocate_position(settings_.get(), file, nullptr);
                    g_list_store_remove(entries.get(), i);
                }
                return;
            }
        }
        if (info)
        {
            update_link_watch(info);
            g_list_store_insert(entries.get(), first, info);
        }
    }

    void update_item(guint index, GFileInfo *current, GFileInfo *next)
    {
        bool visible = gtk_filter_match(filter_.get(), current);
        bool membership_changed = visible != gtk_filter_match(filter_.get(), next);
        bool order_changed = visible && gtk_sorter_compare(sorter_.get(), current, next) != GTK_ORDERING_EQUAL;
        bool previews_enabled = g_settings_get_boolean(settings_.get(), "show-thumbnails");
        bool preview = previews_enabled && can_preview(next);
        bool redraw = g_strcmp0(item_label(current), item_label(next)) != 0 ||
                      !g_icon_equal(g_file_info_get_icon(current), g_file_info_get_icon(next)) ||
                      g_file_info_get_file_type(current) != g_file_info_get_file_type(next) ||
                      g_file_info_get_is_symlink(current) != g_file_info_get_is_symlink(next) ||
                      (previews_enabled && can_preview(current) != preview) ||
                      (preview && ThumbnailVersion(current) != ThumbnailVersion(next));
        g_file_info_copy_into(next, current);
        update_link_watch(current);
        if (membership_changed || order_changed)
        {
            gpointer item = current;
            g_list_store_splice(entries.get(), index, 1, &item, 1);
        }
        if (redraw)
            notify_item_updated(item_updates_.get(), current);
    }

    void update_link_watch(GFileInfo *info)
    {
        watch_symlink_target(info, [weak = weak_from_this(), info]
                             {
            if (auto self = weak.lock())
            {
                self->queue(item_file(info));
                self->read_next();
            } });
    }
};

DesktopDirectory::DesktopDirectory(GFile *location, GtkApplication *app, GSettings *preferences)
    : folder(retain(location)), preferences(retain(preferences)), sorting(preferences), staging(make_file_set()),
      show_thumbnails(g_settings_create_action(preferences, "show-thumbnails")), trash(preferences, item_updates.get()),
      updates_(std::make_shared<Updates>())
{
    g_autoptr(GError) error = nullptr;
    ObjectPtr<GFileMonitor> changes{g_file_monitor_directory(location, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error)};
    if (error)
        notify_error(G_APPLICATION(app), "Could not monitor the Desktop folder", error->message);

    show_hidden.reset(g_simple_action_new_stateful("show-hidden", nullptr, g_variant_new_boolean(false)));
    auto *visibility = new Visibility{FileSet{g_hash_table_ref(staging.get())}, retain(show_hidden.get())};
    auto *filter = gtk_custom_filter_new(visible_item, visibility, [](gpointer data)
                                         { delete static_cast<Visibility *>(data); });
    g_signal_connect_object(show_hidden.get(), "notify::state", G_CALLBACK(+[](GObject *, GParamSpec *, GtkFilter *filter)
                                                                           { gtk_filter_changed(filter, GTK_FILTER_CHANGE_DIFFERENT); }),
                            filter, GConnectFlags{});
    auto *visible = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(updates_->entries.get())), GTK_FILTER(filter));
    ObjectPtr<GtkSortListModel> sorted{gtk_sort_list_model_new(G_LIST_MODEL(visible), GTK_SORTER(g_object_ref(sorting.sorter.get())))};
    auto *sections = g_list_store_new(G_TYPE_LIST_MODEL);
    g_list_store_append(sections, trash.items.get());
    g_list_store_append(sections, sorted.get());
    items.reset(G_LIST_MODEL(gtk_flatten_list_model_new(G_LIST_MODEL(sections))));
    updates_->start(*this, changes.get(), app, GTK_FILTER(filter));
}
