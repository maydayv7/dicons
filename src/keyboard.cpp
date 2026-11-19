#include "keyboard.hpp"
#include "selection_actions.hpp"
#include "inline_rename.hpp"

static gboolean key_pressed(GtkEventControllerKey *, guint key, guint, GdkModifierType state, GtkGridView *grid)
{
    const auto modifiers = state & gtk_accelerator_get_default_mod_mask();
    auto *focus = gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(grid)));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return false;
    if (!modifiers && key == GDK_KEY_Escape && cancel_inline_rename(grid))
        return true;
    if (GTK_IS_EDITABLE(focus) || (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_ENTRY)))
        return false;

    auto *selection = gtk_grid_view_get_model(grid);
    if (modifiers == GDK_CONTROL_MASK && gdk_keyval_to_lower(key) == GDK_KEY_h)
        return gtk_widget_activate_action(GTK_WIDGET(grid), "desktop.show-hidden", nullptr);
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
        rename_selection(grid);
        return true;
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
