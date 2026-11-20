#include "transfer.hpp"
#include "grid_selection.hpp"
#include "icon_grid.hpp"
#include "trash_actions.hpp"
#include "copy.hpp"
#include "copy_stage.hpp"
#include "collision_dialog.hpp"
#include "desktop_directory.hpp"
#include "notifications.hpp"

#include <deque>
#include <memory>

namespace
{
    struct DropSite
    {
        ObjectPtr<GFile> desktop;
        FileSet staging;
    };

    struct Transfer
    {
        ObjectPtr<GtkGridView> grid;
        ObjectPtr<GApplication> app;
        DropSite &site;
        std::deque<ObjectPtr<GFile>> files{};
        ObjectPtr<GFile> stage{};
        bool keep_both{};
        bool copied{};
        bool to_trash{};
    };
}

static void copy_worker(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto *transfer = static_cast<Transfer *>(data);
    g_autoptr(GFile) desktop = g_file_get_parent(transfer->stage.get());
    g_autoptr(GError) error = nullptr;
    auto destination = desktop_copy_item(transfer->files.front().get(), desktop, transfer->stage.get(),
                                         transfer->keep_both, &error);
    if (destination)
        g_task_return_pointer(task, destination.release(), g_object_unref);
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

static void next_item(Transfer *transfer);

static void advance(Transfer *transfer)
{
    transfer->files.pop_front();
    transfer->keep_both = false;
    next_item(transfer);
}

static void copy_finished(GObject *, GAsyncResult *result, gpointer data)
{
    auto *transfer = static_cast<Transfer *>(data);
    g_autoptr(GError) error = nullptr;
    g_autoptr(GFile) destination = static_cast<GFile *>(g_task_propagate_pointer(G_TASK(result), &error));
    if (destination)
    {
        GtkSelectionModel *selection = gtk_grid_view_get_model(transfer->grid.get());
        if (!transfer->copied)
            gtk_selection_model_unselect_all(selection);
        transfer->copied = true;
        select_when_listed(transfer->grid.get(), destination);
    }
    else
    {
        GFile *file = transfer->files.front().get();
        g_autofree char *path = g_file_get_parse_name(file);
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            auto *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(transfer->grid.get())));
            choose_collision_action(GTK_APPLICATION(transfer->app.get()), parent, path, [transfer](CollisionChoice choice)
                                    {
                if (choice == CollisionChoice::keep_both) {
                    transfer->keep_both = true;
                    next_item(transfer);
                } else {
                    advance(transfer);
                } });
            return;
        }
        g_autofree char *detail = g_strdup_printf("%s\n%s", path, error->message);
        notify_error(transfer->app.get(), "Could not copy item", detail);
    }
    advance(transfer);
}

static void next_item(Transfer *transfer)
{
    if (!transfer->files.empty())
    {
        g_autoptr(GTask) task = g_task_new(nullptr, nullptr, copy_finished, transfer);
        g_task_set_task_data(task, transfer, nullptr);
        g_task_run_in_thread(task, copy_worker);
        return;
    }

    if (transfer->stage)
        remove_copy_stage(transfer->stage.get(), transfer->site.staging.get(), transfer->app.get());
    delete transfer;
}

static bool prepare_copy(Transfer &transfer, const GValue *value)
{
    auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
    GSList *files = gdk_file_list_get_files(list);
    if (!files)
    {
        notify_error(transfer.app.get(), "Could not receive dropped files", "The file list was empty.");
        return false;
    }

    transfer.stage = make_copy_stage(transfer.site.desktop.get(), transfer.site.staging.get(), transfer.app.get());
    if (!transfer.stage)
        return false;
    for (GSList *link = files; link; link = link->next)
        transfer.files.push_back(retain(G_FILE(link->data)));
    return true;
}

static void files_received(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<Transfer> transfer{static_cast<Transfer *>(data)};
    auto *drop = GDK_DROP(source);
    g_autoptr(GError) error = nullptr;
    const GValue *value = gdk_drop_read_value_finish(drop, result, &error);
    if (!value)
    {
        gdk_drop_finish(drop, GdkDragAction{});
        notify_error(transfer->app.get(), "Could not receive dropped files", error->message);
        return;
    }
    if (transfer->to_trash)
    {
        auto *origin = drag_origin(drop);
        auto *grid = origin && gtk_widget_get_root(GTK_WIDGET(origin)) ? origin : transfer->grid.get();
        if (!gtk_widget_get_root(GTK_WIDGET(grid)))
        {
            gdk_drop_finish(drop, GdkDragAction{});
            return;
        }
        auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
        std::vector<GFile *> files;
        for (auto *link = gdk_file_list_get_files(list); link; link = link->next)
            files.push_back(G_FILE(link->data));
        // Our source offers COPY to other apps
        // Here we own both ends of the drag
        auto action = origin ? GDK_ACTION_COPY : GDK_ACTION_MOVE;
        trash_files(grid, files, [drop = GDK_DROP(g_object_ref(drop)), action](bool ok)
                    {
            gdk_drop_finish(drop, ok ? action : GdkDragAction{});
            g_object_unref(drop); });
        return;
    }
    // Release before collision resolution
    const bool ready = prepare_copy(*transfer, value);
    const bool can_copy = (gdk_drop_get_actions(drop) & GDK_ACTION_COPY) != 0;
    gdk_drop_finish(drop, ready && can_copy ? GDK_ACTION_COPY : GdkDragAction{});
    if (ready)
        next_item(transfer.release());
}

static gboolean accepts_files(GtkDropTargetAsync *target, GdkDrop *drop, gpointer)
{
    return drag_origin(drop) || gdk_content_formats_match(gtk_drop_target_async_get_formats(target), gdk_drop_get_formats(drop));
}

static bool over_trash(GtkDropTargetAsync *target, double x, double y, GtkGridView *grid)
{
    auto *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    auto *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
    auto *item = target_item(picked, grid);
    return item && (picked == tile_content(item) || gtk_widget_is_ancestor(picked, tile_content(item))) &&
           is_trash_icon(G_FILE_INFO(gtk_list_item_get_item(item)));
}

static GdkDragAction drop_action(GtkDropTargetAsync *target, GdkDrop *drop, double x, double y, GtkGridView *grid)
{
    bool ours = drag_origin(drop) != nullptr;
    if (!ours && over_trash(target, x, y, grid))
        return GdkDragAction(gdk_drop_get_actions(drop) & GDK_ACTION_MOVE);
    return GDK_ACTION_COPY;
}

static gboolean accept_drop(GtkDropTargetAsync *target, GdkDrop *drop, double x, double y, GtkGridView *grid)
{
    if (!drop_action(target, drop, x, y, grid))
        return false;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
    bool to_trash = over_trash(target, x, y, grid);
    if (drag_origin(drop) && (!to_trash || dragging_trash(drop)))
    {
        bool moved = move_icons(drop, grid, GTK_WIDGET(window), x, y);
        gdk_drop_finish(drop, moved ? GDK_ACTION_COPY : GdkDragAction{});
        return true;
    }
    auto *selection = gtk_grid_view_get_model(grid);
    auto *site = static_cast<DropSite *>(g_object_get_data(G_OBJECT(selection), "drop-site"));
    auto *transfer = new Transfer{
        .grid = retain(grid),
        .app = retain(G_APPLICATION(gtk_window_get_application(window))),
        .site = *site,
        .to_trash = to_trash,
    };
    gdk_drop_read_value_async(drop, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, nullptr, files_received, transfer);
    return TRUE;
}

void transfer_attach(GtkWindow *window, GtkGridView *grid, GFile *desktop, GHashTable *staging)
{
    auto *site = new DropSite{retain(desktop), FileSet{g_hash_table_ref(staging)}};
    GtkSelectionModel *selection = gtk_grid_view_get_model(grid);
    g_object_set_data_full(G_OBJECT(selection), "drop-site", site, [](gpointer data)
                           { delete static_cast<DropSite *>(data); });

    GdkContentFormats *formats = gdk_content_formats_new_for_gtype(GDK_TYPE_FILE_LIST);
    formats = gdk_content_formats_union_deserialize_mime_types(formats);
    GtkDropTargetAsync *target = gtk_drop_target_async_new(formats, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE));
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(target), GTK_PHASE_CAPTURE);
    g_signal_connect(target, "accept", G_CALLBACK(accepts_files), nullptr);
    g_signal_connect_object(target, "drag-enter", G_CALLBACK(drop_action), grid, GConnectFlags{});
    g_signal_connect_object(target, "drag-motion", G_CALLBACK(drop_action), grid, GConnectFlags{});
    g_signal_connect_object(target, "drop", G_CALLBACK(accept_drop), grid, GConnectFlags{});
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(target));
}
