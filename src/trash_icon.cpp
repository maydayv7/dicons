#include "trash_icon.hpp"

bool is_trash_icon(GFileInfo *info)
{
    return g_file_info_get_attribute_boolean(info, "dicons::trash");
}

static void set_icon(GFileInfo *info, bool full)
{
    ObjectPtr<GIcon> icon{g_themed_icon_new(full ? "user-trash-full" : "user-trash")};
    g_themed_icon_append_name(G_THEMED_ICON(icon.get()), full ? "user-trash-full-symbolic" : "user-trash-symbolic");
    if (full)
    {
        g_themed_icon_append_name(G_THEMED_ICON(icon.get()), "user-trash");
        g_themed_icon_append_name(G_THEMED_ICON(icon.get()), "user-trash-symbolic");
    }
    g_file_info_set_icon(info, icon.get());
}

TrashIcon::TrashIcon(GSettings *preferences)
    : visible(g_settings_create_action(preferences, "show-trash")), location_(g_file_new_for_uri("trash:///"))
{
    g_file_info_set_name(info_.get(), "Trash");
    g_file_info_set_display_name(info_.get(), "Trash");
    g_file_info_set_file_type(info_.get(), G_FILE_TYPE_DIRECTORY);
    g_file_info_set_is_symlink(info_.get(), false);
    g_file_info_set_attribute_boolean(info_.get(), "dicons::trash", true);
    g_file_info_set_attribute_object(info_.get(), "standard::file", G_OBJECT(location_.get()));
    set_icon(info_.get(), false);
    visibility_changed(visible.get(), nullptr, this);
    g_signal_connect(visible.get(), "notify::state", G_CALLBACK(visibility_changed), this);

    monitor_.reset(g_file_monitor_directory(location_.get(), G_FILE_MONITOR_NONE, nullptr, nullptr));
    if (!monitor_)
    {
        // GIO can trash native files even without a trash:/// provider
        local_ = true;
        location_.reset(g_file_new_build_filename(g_get_user_data_dir(), "Trash", "files", nullptr));
        watch_local();
    }
    else
        g_signal_connect(monitor_.get(), "changed", G_CALLBACK(+[](GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, TrashIcon *self)
                                                               { self->refresh(); }),
                         this);
    refresh();
}

void TrashIcon::visibility_changed(GAction *action, GParamSpec *, TrashIcon *self)
{
    g_autoptr(GVariant) state = g_action_get_state(action);
    if (!g_variant_get_boolean(state))
        g_list_store_remove_all(self->items.get());
    else if (!g_list_model_get_n_items(G_LIST_MODEL(self->items.get())))
        g_list_store_append(self->items.get(), self->info_.get());
}

void TrashIcon::watch_local()
{
    // On first use Trash/files may not exist
    // Watch nearest existing ancestor so first deletion updates icon immediately
    auto watched = retain(location_.get());
    while (g_file_query_file_type(watched.get(), G_FILE_QUERY_INFO_NONE, nullptr) != G_FILE_TYPE_DIRECTORY)
    {
        ObjectPtr<GFile> parent{g_file_get_parent(watched.get())};
        if (!parent)
            return;
        watched = std::move(parent);
    }
    if (monitor_)
        g_signal_handlers_disconnect_by_data(monitor_.get(), this);
    monitor_.reset(g_file_monitor_directory(watched.get(), G_FILE_MONITOR_NONE, nullptr, nullptr));
    if (monitor_)
        g_signal_connect(monitor_.get(), "changed", G_CALLBACK(+[](GFileMonitor *, GFile *file, GFile *, GFileMonitorEvent, TrashIcon *self)
                                                               {
                                                                   if (g_file_equal(self->location_.get(), file) || g_file_has_prefix(self->location_.get(), file))
                                                                       self->watch_local();
                                                                   else if (!g_file_has_parent(file, self->location_.get()))
                                                                       return;
                                                                   self->refresh();
                                                               }),
                         this);
}

TrashIcon::~TrashIcon()
{
    g_signal_handlers_disconnect_by_data(visible.get(), this);
    if (monitor_)
        g_signal_handlers_disconnect_by_data(monitor_.get(), this);
    g_cancellable_cancel(cancel_.get());
}

static void read_trash_state(GTask *task, gpointer source, gpointer local, GCancellable *cancel)
{
    g_autoptr(GError) error = nullptr;
    bool full = false;
    if (local)
    {
        ObjectPtr<GFileEnumerator> entries{g_file_enumerate_children(G_FILE(source), "standard::name",
                                                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, &error)};
        if (entries)
        {
            ObjectPtr<GFileInfo> first{g_file_enumerator_next_file(entries.get(), cancel, &error)};
            full = bool(first);
        }
    }
    else
    {
        ObjectPtr<GFileInfo> count{g_file_query_info(G_FILE(source), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT,
                                                     G_FILE_QUERY_INFO_NONE, cancel, &error)};
        full = count && g_file_info_get_attribute_uint32(count.get(), G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT) > 0;
    }
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_task_return_error(task, g_steal_pointer(&error));
    else
        g_task_return_boolean(task, full);
}

void TrashIcon::state_read(GObject *, GAsyncResult *result, gpointer data)
{
    auto *task = G_TASK(result);
    if (g_cancellable_is_cancelled(g_task_get_cancellable(task)))
        return;
    auto *self = static_cast<TrashIcon *>(data);
    self->reading_ = false;
    if (self->dirty_)
    {
        self->dirty_ = false;
        self->refresh();
        return;
    }
    g_autoptr(GError) error = nullptr;
    bool full = g_task_propagate_boolean(task, &error);
    if (error || full == static_cast<bool>(g_file_info_get_attribute_boolean(self->info_.get(), "dicons::full")))
        return;
    g_file_info_set_attribute_boolean(self->info_.get(), "dicons::full", full);
    set_icon(self->info_.get(), full);
    if (g_list_model_get_n_items(G_LIST_MODEL(self->items.get())))
    {
        gpointer item = self->info_.get();
        g_list_store_splice(self->items.get(), 0, 1, &item, 1);
    }
}

void TrashIcon::refresh()
{
    if (reading_)
    {
        dirty_ = true;
        return;
    }
    reading_ = true;
    ObjectPtr<GTask> task{g_task_new(location_.get(), cancel_.get(), state_read, this)};
    g_task_set_task_data(task.get(), GINT_TO_POINTER(local_), nullptr);
    g_task_run_in_thread(task.get(), read_trash_state);
}
