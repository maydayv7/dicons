#include "transfer.hpp"
#include "grid_selection.hpp"
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
    const bool ready = value && prepare_copy(*transfer, value);

    // Release the pointer before copy resolution
    const bool can_copy = (gdk_drop_get_actions(drop) & GDK_ACTION_COPY) != 0;
    gdk_drop_finish(drop, ready && can_copy ? GDK_ACTION_COPY : GdkDragAction{});

    if (ready)
        next_item(transfer.release());
    else if (!value)
        notify_error(transfer->app.get(), "Could not receive dropped files", error->message);
}

// Some compositors preselect MOVE before the destination has offered its actions
// Negotiate COPY for file offers and let Wayland match it against the source
static gboolean accepts_files(GtkDropTargetAsync *target, GdkDrop *drop, gpointer)
{
    return gdk_content_formats_match(gtk_drop_target_async_get_formats(target), gdk_drop_get_formats(drop));
}

static GdkDragAction prefer_copy(GtkDropTargetAsync *, GdkDrop *, double, double, gpointer)
{
    return GDK_ACTION_COPY;
}

static gboolean accept_drop(GtkDropTargetAsync *, GdkDrop *drop, double, double, GtkGridView *grid)
{
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
    auto *selection = gtk_grid_view_get_model(grid);
    auto *site = static_cast<DropSite *>(g_object_get_data(G_OBJECT(selection), "drop-site"));
    auto *transfer = new Transfer{
        .grid = retain(grid),
        .app = retain(G_APPLICATION(gtk_window_get_application(window))),
        .site = *site,
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
    GtkDropTargetAsync *target = gtk_drop_target_async_new(formats, GDK_ACTION_COPY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(target), GTK_PHASE_CAPTURE);
    g_signal_connect(target, "accept", G_CALLBACK(accepts_files), nullptr);
    g_signal_connect(target, "drag-enter", G_CALLBACK(prefer_copy), nullptr);
    g_signal_connect(target, "drag-motion", G_CALLBACK(prefer_copy), nullptr);
    g_signal_connect_object(target, "drop", G_CALLBACK(accept_drop), grid, GConnectFlags{});
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(target));
}
