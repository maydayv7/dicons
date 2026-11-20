#include "keyboard.hpp"
#include "trash_actions.hpp"
#include "selection_actions.hpp"
#include "inline_rename.hpp"

#include <algorithm>

static void zoom(GSettings *settings, int step)
{
    int size = g_settings_get_int(settings, "icon-size");
    g_settings_set_int(settings, "icon-size", std::clamp(size + step * 8, 24, 128));
}

static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint, GdkModifierType state, GtkGridView *grid)
{
    const auto modifiers = state & gtk_accelerator_get_default_mod_mask();
    auto *focus = gtk_root_get_focus(gtk_widget_get_root(GTK_WIDGET(grid)));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return false;
    if (!modifiers && key == GDK_KEY_Escape && cancel_inline_rename(grid))
        return true;
    if (GTK_IS_EDITABLE(focus) || (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_ENTRY)))
        return false;

    auto *settings = static_cast<GSettings *>(g_object_get_data(G_OBJECT(controller), "settings"));
    if (modifiers == GDK_CONTROL_MASK || modifiers == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
    {
        if (key == GDK_KEY_plus || key == GDK_KEY_KP_Add || (key == GDK_KEY_equal && modifiers == GDK_CONTROL_MASK))
        {
            zoom(settings, 1);
            return true;
        }
        if (modifiers == GDK_CONTROL_MASK && (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract))
        {
            zoom(settings, -1);
            return true;
        }
    }

    auto *selection = gtk_grid_view_get_model(grid);
    if (modifiers == GDK_CONTROL_MASK && gdk_keyval_to_lower(key) == GDK_KEY_z)
    {
        undo_trash(grid);
        return true;
    }
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

static gboolean scroll_zoom(GtkEventControllerScroll *controller, double, double dy, GSettings *settings)
{
    auto *event = GTK_EVENT_CONTROLLER(controller);
    auto mods = gtk_event_controller_get_current_event_state(event) & gtk_accelerator_get_default_mod_mask();
    if (mods != GDK_CONTROL_MASK || !dy)
        return false;
    zoom(settings, dy < 0 ? 1 : -1);
    return true;
}

void attach_desktop_keys(GtkWindow *window, GtkGridView *grid, GSettings *settings)
{
    attach_inline_rename(window, grid);
    auto *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect_object(keys, "key-pressed", G_CALLBACK(key_pressed), grid, GConnectFlags{});
    g_object_set_data_full(G_OBJECT(keys), "settings", g_object_ref(settings), g_object_unref);
    gtk_widget_add_controller(GTK_WIDGET(window), keys);

    auto *scroll = gtk_event_controller_scroll_new(GtkEventControllerScrollFlags(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                                                                 GTK_EVENT_CONTROLLER_SCROLL_DISCRETE));
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect_object(scroll, "scroll", G_CALLBACK(scroll_zoom), settings, GConnectFlags{});
    gtk_widget_add_controller(GTK_WIDGET(grid), scroll);
}
