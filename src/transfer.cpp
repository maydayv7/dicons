#include "undo.hpp"
#include "transfer.hpp"
#include "grid_selection.hpp"
#include "icon_grid.hpp"
#include "trash_actions.hpp"
#include "copy.hpp"
#include "file_rename.hpp"
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
        ObjectPtr<GFile> folder;
        ObjectPtr<GdkDrop> drop;
        UndoToken undo;
        GdkDragAction action;
        bool move;
        bool complete = true;
        DropIdentity target_identity;
        std::shared_ptr<const DragIdentities> source_identities;
        std::deque<ObjectPtr<GFile>> files{};
        ObjectPtr<GFile> stage{};
        bool keep_both{};
        bool copied{};
        bool to_trash{};
    };
}

static void transfer_worker(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto *transfer = static_cast<Transfer *>(data);
    auto *source = transfer->files.front().get();
    g_autoptr(GError) error = nullptr;
    if (transfer->move && !transfer->source_identities)
    {
        auto identities = std::make_shared<DragIdentities>();
        for (const auto &file : transfer->files)
        {
            g_autofree char *uri = g_file_get_uri(file.get());
            identities->emplace(uri, file_identity(file.get()));
        }
        transfer->source_identities = std::move(identities);
    }
    if ((transfer->folder && !transfer->target_identity.matches(transfer->folder.get(), G_FILE_QUERY_INFO_NONE, &error)) ||
        (transfer->source_identities && !matches_drag_source(*transfer->source_identities, source, &error)))
    {
        g_task_return_error(task, g_steal_pointer(&error));
        return;
    }
    auto *target = transfer->folder ? transfer->folder.get() : transfer->site.desktop.get();
    auto destination = transfer->move ? move_into_folder(source, target, transfer->keep_both, &error)
                                      : desktop_copy_item(source, target, transfer->stage.get(), transfer->keep_both, &error);
    if (destination)
    {
        if (transfer->move && g_file_equal(destination.get(), source))
            transfer->complete = false;
        else if (transfer->move)
            remember_file(transfer->undo, destination.get(), source);
        else
            remember_file(transfer->undo, destination.get());
        g_task_return_pointer(task, destination.release(), g_object_unref);
    }
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

static void transfer_finished(GObject *, GAsyncResult *result, gpointer data)
{
    auto *transfer = static_cast<Transfer *>(data);
    g_autoptr(GError) error = nullptr;
    g_autoptr(GFile) destination = static_cast<GFile *>(g_task_propagate_pointer(G_TASK(result), &error));
    if (destination && !transfer->folder && gtk_widget_get_root(GTK_WIDGET(transfer->grid.get())))
    {
        GtkSelectionModel *selection = gtk_grid_view_get_model(transfer->grid.get());
        if (!transfer->copied)
            gtk_selection_model_unselect_all(selection);
        transfer->copied = true;
        select_when_listed(transfer->grid.get(), destination);
    }
    else if (!destination)
    {
        GFile *file = transfer->files.front().get();
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            g_autofree char *name = g_file_get_basename(file);
            auto *folder = transfer->folder ? transfer->folder.get() : transfer->site.desktop.get();
            ObjectPtr<GFile> conflict{g_file_get_child(folder, name)};
            g_autofree char *path = g_file_get_parse_name(conflict.get());
            auto *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(transfer->grid.get())));
            choose_collision_action(GTK_APPLICATION(transfer->app.get()), parent, path, [transfer](CollisionChoice choice)
                                    {
                if (choice == CollisionChoice::keep_both) {
                    transfer->keep_both = true;
                    next_item(transfer);
                } else {
                    transfer->complete = false;
                    advance(transfer);
                } });
            return;
        }
        transfer->complete = false;
        notify_file_error(transfer->app.get(), transfer->move ? "Could not move item" : "Could not copy item", file, error);
    }
    advance(transfer);
}

static void next_item(Transfer *transfer)
{
    if (!transfer->files.empty())
    {
        g_autoptr(GTask) task = g_task_new(nullptr, nullptr, transfer_finished, transfer);
        g_task_set_task_data(task, transfer, nullptr);
        g_task_run_in_thread(task, transfer_worker);
        return;
    }

    if (transfer->stage)
        remove_copy_stage(transfer->stage.get(), transfer->site.staging.get(), transfer->app.get());
    finish_undo_step(transfer->app.get(), transfer->undo);
    if (transfer->drop)
        gdk_drop_finish(transfer->drop.get(), transfer->complete ? transfer->action : GdkDragAction{});
    delete transfer;
}

static bool prepare_transfer(Transfer &transfer, const GValue *value)
{
    auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
    GSList *files = gdk_file_list_get_files(list);
    if (!files)
    {
        notify_error(transfer.app.get(), "Could not receive dropped files", "The file list was empty.");
        return false;
    }

    if (!transfer.move)
    {
        transfer.stage = make_copy_stage(transfer.folder ? transfer.folder.get() : transfer.site.desktop.get(), transfer.site.staging.get(), transfer.app.get());
        if (!transfer.stage)
            return false;
    }
    for (GSList *link = files; link; link = link->next)
        transfer.files.push_back(retain(G_FILE(link->data)));
    return true;
}

static void receive_files(Transfer *data, const GValue *value, GError *error)
{
    std::unique_ptr<Transfer> transfer{data};
    auto *drop = transfer->drop.get();
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
            g_object_unref(drop); }, transfer->source_identities);
        return;
    }
    if (!prepare_transfer(*transfer, value))
    {
        gdk_drop_finish(drop, GdkDragAction{});
        return;
    }
    transfer->undo = begin_undo_step(transfer->app.get());
    // Keep a MOVE offer alive until the files have actually moved
    if (transfer->action == GDK_ACTION_COPY)
    {
        gdk_drop_finish(drop, GDK_ACTION_COPY);
        transfer->drop.reset();
    }
    next_item(transfer.release());
}

static void files_received(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = nullptr;
    auto *value = gdk_drop_read_value_finish(GDK_DROP(source), result, &error);
    receive_files(static_cast<Transfer *>(data), value, error);
}

static void uris_decoded(GObject *, GAsyncResult *result, gpointer data)
{
    g_auto(GValue) value = G_VALUE_INIT;
    g_autoptr(GError) error = nullptr;
    bool ok = gdk_content_deserialize_finish(result, &value, &error);
    receive_files(static_cast<Transfer *>(data), ok ? &value : nullptr, error);
}

static void uri_stream_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GError) error = nullptr;
    const char *mime = nullptr;
    ObjectPtr<GInputStream> stream{gdk_drop_read_finish(GDK_DROP(source), result, &mime, &error)};
    if (stream)
        gdk_content_deserialize_async(stream.get(), mime, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, nullptr, uris_decoded, data);
    else
        receive_files(static_cast<Transfer *>(data), nullptr, error);
}

static gboolean accepts_files(GtkDropTargetAsync *target, GdkDrop *drop, gpointer)
{
    return drag_origin(drop) || gdk_content_formats_match(gtk_drop_target_async_get_formats(target), gdk_drop_get_formats(drop));
}

static GFileInfo *drop_item(GtkDropTargetAsync *target, double x, double y, GtkGridView *grid)
{
    auto *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    auto *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
    auto *item = target_item(picked, grid);
    if (!item)
        return nullptr;
    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    if (is_trash_icon(info) && picked != tile_content(item) && !gtk_widget_is_ancestor(picked, tile_content(item)))
        return nullptr;
    return info;
}

static GdkDragAction drop_action(GtkDropTargetAsync *target, GdkDrop *drop, double x, double y, GtkGridView *grid)
{
    auto *info = drop_item(target, x, y, grid);
    auto offered = gdk_drop_get_actions(drop);
    if (drag_origin(drop))
        return GdkDragAction(offered & GDK_ACTION_COPY);
    if (info && is_trash_icon(info))
        return GdkDragAction(offered & GDK_ACTION_MOVE);
    return (offered & GDK_ACTION_COPY) ? GDK_ACTION_COPY : GdkDragAction(offered & GDK_ACTION_MOVE);
}

static gboolean accept_drop(GtkDropTargetAsync *target, GdkDrop *drop, double x, double y, GtkGridView *grid)
{
    auto action = drop_action(target, drop, x, y, grid);
    if (!action)
        return false;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
    auto *info = drop_item(target, x, y, grid);
    bool to_trash = info && is_trash_icon(info);
    ObjectPtr<GFile> folder;
    auto identities = drag_identities(drop);
    if (identities)
    {
        if (info && !to_trash && !dragging_trash(drop) && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
        {
            g_autofree char *uri = g_file_get_uri(item_file(info));
            if (!identities->contains(uri))
                folder = retain(item_file(info));
        }
        if (!folder && (!to_trash || dragging_trash(drop)))
        {
            bool moved = move_icons(drop, grid, GTK_WIDGET(window), x, y);
            gdk_drop_finish(drop, moved ? GDK_ACTION_COPY : GdkDragAction{});
            return true;
        }
    }
    else if (info && !to_trash && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
        folder = retain(item_file(info));
    auto *selection = gtk_grid_view_get_model(grid);
    auto *site = static_cast<DropSite *>(g_object_get_data(G_OBJECT(selection), "drop-site"));
    auto *transfer = new Transfer{
        .grid = retain(grid),
        .app = retain(G_APPLICATION(gtk_window_get_application(window))),
        .site = *site,
        .folder = std::move(folder),
        .drop = retain(drop),
        .action = action,
        .move = bool(identities) || action == GDK_ACTION_MOVE,
        .target_identity = DropIdentity(info),
        .source_identities = std::move(identities),
        .to_trash = to_trash,
    };
    // Prefer the file manager's original URIs to a portal export when it offers both.
    static const char *uri_formats[] = {"text/uri-list", nullptr};
    if (!transfer->source_identities && gdk_content_formats_contain_mime_type(gdk_drop_get_formats(drop), uri_formats[0]))
        gdk_drop_read_async(drop, uri_formats, G_PRIORITY_DEFAULT, nullptr, uri_stream_ready, transfer);
    else
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
