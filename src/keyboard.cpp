#include "keyboard.hpp"
#include "desktop_directory.hpp"
#include "grid_selection.hpp"
#include "inline_rename.hpp"
#include "notifications.hpp"

static GHashTable *pending_trash(GApplication *app)
{
    auto *pending = static_cast<GHashTable *>(g_object_get_data(G_OBJECT(app), "pending-trash"));
    if (!pending)
    {
        pending = make_file_set().release();
        g_object_set_data_full(G_OBJECT(app), "pending-trash", pending, [](gpointer p)
                               { g_hash_table_unref(static_cast<GHashTable *>(p)); });
    }
    return pending;
}

static void trash_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    ObjectPtr<GApplication> app{G_APPLICATION(data)};
    GFile *file = G_FILE(source);
    g_hash_table_remove(pending_trash(app.get()), file);
    g_autoptr(GError) error = nullptr;
    if (!g_file_trash_finish(file, result, &error))
        notify_file_error(app.get(), "Could not move item to Trash", file, error);
}

static void trash_selection(GtkGridView *grid)
{
    auto *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
    auto *app = G_APPLICATION(gtk_window_get_application(window));
    auto *pending = pending_trash(app);
    for (GFile *file : selected_files(gtk_grid_view_get_model(grid)))
        if (g_hash_table_add(pending, g_object_ref(file)))
            g_file_trash_async(file, G_PRIORITY_DEFAULT, nullptr, trash_finished, g_object_ref(app));
}

static gboolean key_pressed(GtkEventControllerKey *, guint key, guint, GdkModifierType state, GtkGridView *grid)
{
    const auto modifiers = state & gtk_accelerator_get_default_mod_mask();
    if (!modifiers && key == GDK_KEY_Escape && cancel_inline_rename(grid))
        return true;

    auto *focus = gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(grid)));
    if (GTK_IS_EDITABLE(focus) || (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_ENTRY)))
        return false;

    auto *selection = gtk_grid_view_get_model(grid);
    if (modifiers == GDK_CONTROL_MASK && gdk_keyval_to_lower(key) == GDK_KEY_a)
        return gtk_selection_model_select_all(selection);
    if (modifiers)
        return false;
    switch (key)
    {
    case GDK_KEY_Escape:
        gtk_selection_model_unselect_all(selection);
        return true;
    case GDK_KEY_F2:
    {
        g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
        if (gtk_bitset_get_size(selected) == 1)
            start_inline_rename(grid, gtk_bitset_get_minimum(selected));
        return true;
    }
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        trash_selection(grid);
        return true;
    default:
        return false; // GTK handles navigation and Enter on the focused icon
    }
}

void attach_desktop_keys(GtkWindow *window, GtkGridView *grid)
{
    attach_inline_rename(window, grid);
    auto *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect_object(keys, "key-pressed", G_CALLBACK(key_pressed), grid, GConnectFlags{});
    gtk_widget_add_controller(GTK_WIDGET(window), keys);
}
