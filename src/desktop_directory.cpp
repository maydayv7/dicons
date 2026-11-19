#include "desktop_directory.hpp"
#include "notifications.hpp"

#include <deque>

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

    void start(GFile *folder, GFileMonitor *monitor, GtkApplication *app)
    {
        folder_ = retain(folder);
        if (monitor)
        {
            monitor_ = retain(monitor);
            g_signal_connect(monitor, "changed", G_CALLBACK(directory_changed), this);
        }
        scan_.reset(gtk_directory_list_new(attributes, nullptr));
        gtk_directory_list_set_monitored(scan_.get(), false);
        g_signal_connect(scan_.get(), "items-changed", G_CALLBACK(scanned_items), entries.get());
        g_signal_connect(scan_.get(), "notify::loading", G_CALLBACK(scan_progress), this);
        g_signal_connect_object(scan_.get(), "notify::error", G_CALLBACK(directory_error), app, GConnectFlags{});
        gtk_directory_list_set_file(scan_.get(), folder);
    }

    ~Updates()
    {
        if (monitor_)
            g_signal_handlers_disconnect_by_data(monitor_.get(), this);
        if (scan_)
        {
            g_signal_handlers_disconnect_by_data(scan_.get(), this);
            g_signal_handlers_disconnect_by_data(scan_.get(), entries.get());
        }
    }

private:
    static constexpr auto attributes = "standard::name,standard::display-name,standard::edit-name,standard::icon,standard::type,standard::is-hidden";
    ObjectPtr<GtkDirectoryList> scan_;
    ObjectPtr<GFile> folder_;
    ObjectPtr<GFileMonitor> monitor_;
    std::deque<ObjectPtr<GFile>> pending_;
    FileSet queued_ = make_file_set();
    bool reading_{};

    static void scanned_items(GListModel *scan, guint position, guint removed, guint added, GListStore *entries)
    {
        g_autoptr(GPtrArray) batch = g_ptr_array_new_with_free_func(g_object_unref);
        for (guint i = position; i < position + added; ++i)
            g_ptr_array_add(batch, g_list_model_get_item(scan, i));
        g_list_store_splice(entries, position, removed, batch->pdata, batch->len);
    }

    static void scan_progress(GtkDirectoryList *scan, GParamSpec *, Updates *self)
    {
        if (gtk_directory_list_is_loading(scan))
            return;
        g_signal_handlers_disconnect_by_data(scan, self->entries.get());
        g_signal_handlers_disconnect_by_data(scan, self);
        self->scan_.reset();
        self->read_next();
    }

    static void directory_changed(GFileMonitor *, GFile *file, GFile *other,
                                  GFileMonitorEvent event, Updates *self)
    {
        switch (event)
        {
        case G_FILE_MONITOR_EVENT_RENAMED:
            self->queue(file);
            self->queue(other);
            break;
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_DELETED:
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
        g_file_query_info_async(file.get(), attributes, G_FILE_QUERY_INFO_NONE, G_PRIORITY_DEFAULT,
                                nullptr, file_read, new std::weak_ptr<Updates>(weak_from_this()));
    }

    static void file_read(GObject *source, GAsyncResult *result, gpointer data)
    {
        std::unique_ptr<std::weak_ptr<Updates>> lifetime{static_cast<std::weak_ptr<Updates> *>(data)};
        g_autoptr(GError) error = nullptr;
        auto *file = G_FILE(source);
        ObjectPtr<GFileInfo> info{g_file_query_info_finish(file, result, &error)};
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
        if (info)
            g_file_info_set_attribute_object(info, "standard::file", G_OBJECT(file));

        auto *model = G_LIST_MODEL(entries.get());
        for (guint i = 0; i < g_list_model_get_n_items(model); ++i)
        {
            ObjectPtr<GFileInfo> current{G_FILE_INFO(g_list_model_get_item(model, i))};
            if (g_file_equal(item_file(current.get()), file))
            {
                gpointer replacement = info;
                g_list_store_splice(entries.get(), i, 1, &replacement, info ? 1 : 0);
                return;
            }
        }
        if (info)
            g_list_store_append(entries.get(), info);
    }
};

DesktopDirectory::DesktopDirectory(GFile *location, GtkApplication *app)
    : folder(retain(location)), staging(make_file_set()), updates_(std::make_shared<Updates>())
{
    g_autoptr(GError) error = nullptr;
    changes.reset(g_file_monitor_directory(location, G_FILE_MONITOR_WATCH_MOVES, nullptr, &error));
    if (error)
        notify_error(G_APPLICATION(app), "Could not monitor the Desktop folder", error->message);

    show_hidden.reset(g_simple_action_new_stateful("show-hidden", nullptr, g_variant_new_boolean(false)));
    auto *visibility = new Visibility{FileSet{g_hash_table_ref(staging.get())}, retain(show_hidden.get())};
    auto *filter = gtk_custom_filter_new(visible_item, visibility, [](gpointer data)
                                         { delete static_cast<Visibility *>(data); });
    g_signal_connect_object(show_hidden.get(), "notify::state", G_CALLBACK(+[](GObject *, GParamSpec *, GtkFilter *filter)
                                                                           { gtk_filter_changed(filter, GTK_FILTER_CHANGE_DIFFERENT); }),
                            filter, GConnectFlags{});
    items.reset(G_LIST_MODEL(gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(updates_->entries.get())), GTK_FILTER(filter))));
    updates_->start(location, changes.get(), app);
}
