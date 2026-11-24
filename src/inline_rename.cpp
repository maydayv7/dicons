#include "undo.hpp"
#include "inline_rename.hpp"
#include "desktop_directory.hpp"
#include "file_rename.hpp"
#include "grid_selection.hpp"
#include "icon_grid.hpp"
#include "notifications.hpp"

#include <cstring>
#include <utility>

namespace
{
    struct RenameEditor
    {
        GtkListItem *editing{};
        ObjectPtr<GFileInfo> requested;
        DropIdentity identity;
    };

    struct RenameRequest
    {
        ObjectPtr<GtkGridView> grid;
        ObjectPtr<GApplication> app;
        bool keep_selected;
        DropIdentity identity;
        UndoToken undo;
    };
}

static RenameEditor &editor_for(GtkGridView *grid)
{
    return *static_cast<RenameEditor *>(g_object_get_data(G_OBJECT(gtk_grid_view_get_factory(grid)), "rename-editor"));
}

static GtkWidget *tile_label(GtkListItem *item)
{
    auto *box = tile_content(item);
    return gtk_widget_get_next_sibling(gtk_widget_get_first_child(box));
}

static const char *editable_name(GFileInfo *info)
{
    const char *name = g_file_info_get_edit_name(info);
    return name ? name : g_file_info_get_display_name(info);
}

static void close_editor(RenameEditor &editor, bool restore_focus)
{
    auto *item = std::exchange(editor.editing, nullptr);
    if (!item)
        return;
    auto *label = tile_label(item);
    auto *entry = gtk_widget_get_next_sibling(label);
    auto *grid = GTK_GRID_VIEW(gtk_widget_get_ancestor(label, GTK_TYPE_GRID_VIEW));
    gtk_widget_set_visible(label, true);
    if (restore_focus && grid && gtk_list_item_get_position(item) != GTK_INVALID_LIST_POSITION)
        gtk_grid_view_scroll_to(grid, gtk_list_item_get_position(item), GTK_LIST_SCROLL_FOCUS, nullptr);
    gtk_widget_set_visible(entry, false);
}

static void rename_worker(GTask *task, gpointer source, gpointer data, GCancellable *)
{
    g_autoptr(GError) error = nullptr;
    auto *request = static_cast<RenameRequest *>(g_async_result_get_user_data(G_ASYNC_RESULT(task)));
    auto renamed = rename_desktop_item(G_FILE(source), static_cast<const char *>(data), &error, &request->identity);
    if (renamed)
    {
        remember_move(request->undo, renamed.get(), G_FILE(source), request->identity);
        g_task_return_pointer(task, renamed.release(), g_object_unref);
    }
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

static void rename_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<RenameRequest> request{static_cast<RenameRequest *>(data)};
    g_autoptr(GError) error = nullptr;
    ObjectPtr<GFile> renamed{static_cast<GFile *>(g_task_propagate_pointer(G_TASK(result), &error))};
    finish_undo_step(request->app.get(), request->undo);
    if (!renamed)
        notify_file_error(request->app.get(), "Could not rename item", G_FILE(source), error);
    else if (request->keep_selected)
        select_when_listed(request->grid.get(), renamed.get(), ArrivalAction::focus);
}

static void commit_name(RenameEditor &editor, bool restore_focus)
{
    if (!editor.editing)
        return;
    auto *info = G_FILE_INFO(gtk_list_item_get_item(editor.editing));
    auto *entry = gtk_widget_get_next_sibling(tile_label(editor.editing));
    auto *window = GTK_WINDOW(gtk_widget_get_root(entry));
    const char *name = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (window && info && g_strcmp0(name, editable_name(info)) != 0)
    {
        auto *grid = GTK_GRID_VIEW(gtk_widget_get_ancestor(entry, GTK_TYPE_GRID_VIEW));
        auto *request = new RenameRequest{retain(grid), retain(G_APPLICATION(gtk_window_get_application(window))), restore_focus, editor.identity};
        request->undo = begin_undo_step(request->app.get());
        ObjectPtr<GTask> task{g_task_new(item_file(info), nullptr, rename_finished, request)};
        g_task_set_task_data(task.get(), g_strdup(name), g_free);
        g_task_run_in_thread(task.get(), rename_worker);
    }
    close_editor(editor, restore_focus);
}

static void show_editor(RenameEditor &editor, GtkListItem *item)
{
    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    auto *label = tile_label(item);
    auto *entry = gtk_widget_get_next_sibling(label);
    if (!entry)
    {
        entry = gtk_entry_new();
        gtk_editable_set_width_chars(GTK_EDITABLE(entry), 8);
        gtk_editable_set_max_width_chars(GTK_EDITABLE(entry), 12);
        gtk_entry_set_alignment(GTK_ENTRY(entry), 0.5);
        gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Filename", -1);
        g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkEntry *, RenameEditor *state)
                                                       {
                                                           commit_name(*state, true);
                                                       }),
                         &editor);
        auto *focus = gtk_event_controller_focus_new();
        g_signal_connect(focus, "leave", G_CALLBACK(+[](GtkEventControllerFocus *controller, RenameEditor *state)
                                                    {
                                                        if (state->editing && gtk_widget_get_next_sibling(tile_label(state->editing)) ==
                                                                                  gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)))
                                                            commit_name(*state, false);
                                                    }),
                         &editor);
        gtk_widget_add_controller(entry, focus);
        gtk_box_append(GTK_BOX(tile_content(item)), entry);
    }

    editor.editing = item;
    editor.requested.reset();
    const char *name = editable_name(info);
    gtk_editable_set_text(GTK_EDITABLE(entry), name);
    gtk_widget_set_visible(label, false);
    gtk_widget_set_visible(entry, true);
    gtk_widget_grab_focus(entry);
    const char *extension = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY ? nullptr : std::strrchr(name, '.');
    int end = extension && extension != name ? g_utf8_pointer_to_offset(name, extension) : -1;
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, end);
}

static gboolean show_when_bound(GtkWidget *widget, GdkFrameClock *, gpointer)
{
    auto &editor = editor_for(GTK_GRID_VIEW(widget));
    if (!editor.requested)
        return G_SOURCE_REMOVE;
    if (auto *item = bound_item(GTK_GRID_VIEW(widget), editor.requested.get()))
    {
        if (gtk_list_item_get_selected(item))
            show_editor(editor, item);
        editor.requested.reset();
    }
    // Scrolling realizes the target during layout
    return G_SOURCE_REMOVE;
}

static void desktop_pressed(GtkGestureClick *gesture, int, double x, double y, GtkGridView *grid)
{
    auto &editor = editor_for(grid);
    if (!editor.editing)
        return;
    auto *entry = gtk_widget_get_next_sibling(tile_label(editor.editing));
    auto *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    auto *target = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
    if (!target || (target != entry && !gtk_widget_is_ancestor(target, entry)))
        commit_name(editor, false);
}

void attach_inline_rename(GtkWindow *window, GtkGridView *grid)
{
    auto *factory = gtk_grid_view_get_factory(grid);
    auto *state = new RenameEditor;
    g_object_set_data_full(G_OBJECT(factory), "rename-editor", state, [](gpointer p)
                           { delete static_cast<RenameEditor *>(p); });
    g_signal_connect_object(factory, "bind", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *item, GtkGridView *view)
                                                        {
                                                            auto &editor = editor_for(view);
                                                            auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
                                                            if (info && info == editor.requested.get())
                                                                gtk_widget_add_tick_callback(GTK_WIDGET(view), show_when_bound, nullptr, nullptr);
                                                        }),
                            grid, GConnectFlags{});
    g_signal_connect(factory, "unbind", G_CALLBACK(+[](GtkListItemFactory *, GtkListItem *item, RenameEditor *editor)
                                                   {
                                                       if (editor->editing == item)
                                                           close_editor(*editor, false);
                                                   }),
                     state);

    // Empty desktop space can receive a click without taking keyboard focus
    auto *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect_object(click, "pressed", G_CALLBACK(desktop_pressed), grid, GConnectFlags{});
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(click));
}

void start_inline_rename(GtkGridView *grid, guint position)
{
    auto &editor = editor_for(grid);
    auto *model = G_LIST_MODEL(gtk_grid_view_get_model(grid));
    ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(model, position))};
    editor.requested.reset();
    if (!info || is_trash_icon(info.get()) || (editor.editing && gtk_list_item_get_item(editor.editing) == info.get()))
        return;
    if (editor.editing)
        commit_name(editor, false);
    editor.identity = listed_identity(info.get(), item_file(info.get()));
    editor.requested = std::move(info);
    gtk_grid_view_scroll_to(grid, position, GTK_LIST_SCROLL_FOCUS, nullptr);
    if (auto *item = bound_item(grid, editor.requested.get()))
        show_editor(editor, item);
}

bool cancel_inline_rename(GtkGridView *grid)
{
    auto &editor = editor_for(grid);
    const bool active = editor.editing || editor.requested;
    editor.requested.reset();
    close_editor(editor, true);
    return active;
}
