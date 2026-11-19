#include "context_menu.hpp"
#include "create_item.hpp"
#include "desktop_directory.hpp"
#include "launch.hpp"
#include "selection_actions.hpp"
#include "templates.hpp"

namespace
{
    struct DesktopMenu
    {
        GtkGridView *grid;
        GtkWidget *desktop;
        GtkPopoverMenu *popover;
        GtkApplication *app;
        const DesktopDirectory &directory;
    };
}

static GtkListItem *target_item(GtkWidget *widget, GtkGridView *grid)
{
    while (widget && gtk_widget_get_parent(widget) != GTK_WIDGET(grid))
        widget = gtk_widget_get_parent(widget);
    auto *tile = widget ? gtk_widget_get_first_child(widget) : nullptr;
    return tile ? static_cast<GtkListItem *>(g_object_get_data(G_OBJECT(tile), "desktop-item")) : nullptr;
}

static void append_terminal(GMenu *menu, GFile *folder)
{
    g_autofree char *uri = g_file_get_uri(folder);
    g_autoptr(GMenuItem) item = g_menu_item_new("Open in Terminal", nullptr);
    g_menu_item_set_action_and_target(item, "desktop.terminal", "s", uri);
    g_menu_append_item(menu, item);
}

static ObjectPtr<GMenu> background_menu(GFile *folder)
{
    ObjectPtr<GMenu> menu{g_menu_new()};
    g_autoptr(GMenu) create = g_menu_new();
    g_menu_append(create, "Folder", "desktop.folder");
    g_menu_append(create, "Blank File", "desktop.blank");
    append_templates(create);
    g_menu_append_submenu(menu.get(), "Create New", G_MENU_MODEL(create));
    append_sort_menu(menu.get());
    g_menu_append(menu.get(), "Show Hidden Items", "desktop.show-hidden");
    g_menu_append(menu.get(), "Show Thumbnails", "desktop.show-thumbnails");
    append_terminal(menu.get(), folder);
    return menu;
}

static ObjectPtr<GMenu> icon_menu(GtkSelectionModel *selection, GFileInfo *info)
{
    ObjectPtr<GMenu> menu{g_menu_new()};
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
    if (gtk_bitset_get_size(selected) == 1)
    {
        g_menu_append(menu.get(), "Open", "desktop.open");
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
            append_terminal(menu.get(), item_file(info));
        g_menu_append(menu.get(), "Rename", "desktop.rename");
    }
    g_menu_append(menu.get(), "Move to Trash", "desktop.trash");
    return menu;
}

static void popup(DesktopMenu &menu, GtkListItem *item, GdkRectangle anchor)
{
    auto *selection = gtk_grid_view_get_model(menu.grid);
    guint pos = item ? gtk_list_item_get_position(item) : GTK_INVALID_LIST_POSITION;
    bool on_icon = pos != GTK_INVALID_LIST_POSITION;
    if (on_icon)
    {
        if (!gtk_selection_model_is_selected(selection, pos))
            gtk_selection_model_select_item(selection, pos, true);
        gtk_grid_view_scroll_to(menu.grid, pos, GTK_LIST_SCROLL_FOCUS, nullptr);
    }
    else
    {
        gtk_selection_model_unselect_all(selection);
        gtk_widget_grab_focus(menu.desktop);
    }

    auto model = on_icon ? icon_menu(selection, G_FILE_INFO(gtk_list_item_get_item(item)))
                         : background_menu(menu.directory.folder.get());
    gtk_popover_menu_set_menu_model(menu.popover, G_MENU_MODEL(model.get()));
    gtk_popover_set_pointing_to(GTK_POPOVER(menu.popover), &anchor);
    gtk_popover_popup(GTK_POPOVER(menu.popover));
}

static void menu_pressed(GtkGestureClick *gesture, int, double x, double y, DesktopMenu *menu)
{
    auto *target = gtk_widget_pick(menu->desktop, x, y, GTK_PICK_DEFAULT);
    if (target && (gtk_widget_get_ancestor(target, GTK_TYPE_ENTRY) || gtk_widget_get_ancestor(target, GTK_TYPE_POPOVER)))
        return;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    popup(*menu, target_item(target, menu->grid), {static_cast<int>(x), static_cast<int>(y), 1, 1});
}

static gboolean menu_key(GtkEventControllerKey *, guint key, guint, GdkModifierType state, DesktopMenu *menu)
{
    auto mods = state & gtk_accelerator_get_default_mod_mask();
    if (!((key == GDK_KEY_Menu && !mods) || (key == GDK_KEY_F10 && mods == GDK_SHIFT_MASK)))
        return false;

    auto *focus = gtk_root_get_focus(gtk_widget_get_root(menu->desktop));
    if (focus && (gtk_widget_get_ancestor(focus, GTK_TYPE_ENTRY) || gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER)))
        return false;
    auto *item = target_item(focus, menu->grid);
    graphene_rect_t bounds;
    GdkRectangle anchor{8, 8, 1, 1};
    if (item && gtk_widget_compute_bounds(gtk_list_item_get_child(item), menu->desktop, &bounds))
        anchor = {static_cast<int>(bounds.origin.x), static_cast<int>(bounds.origin.y + bounds.size.height), 1, 1};
    popup(*menu, item, anchor);
    return true;
}

static void open_selection(GSimpleAction *, GVariant *, gpointer data)
{
    auto &menu = *static_cast<DesktopMenu *>(data);
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(gtk_grid_view_get_model(menu.grid));
    if (gtk_bitset_get_size(selected) == 1)
        open_item(menu.grid, gtk_bitset_get_minimum(selected), menu.app);
}

void attach_context_menu(GtkWindow *window, GtkGridView *grid, const DesktopDirectory &directory)
{
    auto *desktop = gtk_window_get_child(window);
    gtk_widget_set_focusable(desktop, true);
    auto *popover = gtk_popover_menu_new_from_model(nullptr);
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), false);
    gtk_overlay_add_overlay(GTK_OVERLAY(desktop), popover);
    auto *menu = new DesktopMenu{grid, desktop, GTK_POPOVER_MENU(popover), gtk_window_get_application(window),
                                 directory};
    g_object_set_data_full(G_OBJECT(desktop), "desktop-menu", menu, [](gpointer p)
                           { delete static_cast<DesktopMenu *>(p); });

    g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
    const GActionEntry actions[] = {
        {"open", open_selection},
        {"rename", +[](GSimpleAction *, GVariant *, gpointer data)
                   { rename_selection(static_cast<DesktopMenu *>(data)->grid); }},
        {"trash", +[](GSimpleAction *, GVariant *, gpointer data)
                  { trash_selection(static_cast<DesktopMenu *>(data)->grid); }},
        {"folder", +[](GSimpleAction *, GVariant *, gpointer data)
                   {
                       auto &menu = *static_cast<DesktopMenu *>(data);
                       create_item(menu.grid, menu.directory, NewItem::folder);
                   }},
        {"blank", +[](GSimpleAction *, GVariant *, gpointer data)
                  {
                      auto &menu = *static_cast<DesktopMenu *>(data);
                      create_item(menu.grid, menu.directory, NewItem::blank);
                  }},
        {"template", +[](GSimpleAction *, GVariant *value, gpointer data)
                     {
                         auto &menu = *static_cast<DesktopMenu *>(data);
                         ObjectPtr<GFile> file{g_file_new_for_uri(g_variant_get_string(value, nullptr))};
                         create_item(menu.grid, menu.directory, NewItem::from_template, file.get());
                     },
         "s"},
        {"terminal", +[](GSimpleAction *, GVariant *value, gpointer data)
                     {
                         auto &menu = *static_cast<DesktopMenu *>(data);
                         ObjectPtr<GFile> folder{g_file_new_for_uri(g_variant_get_string(value, nullptr))};
                         open_terminal(menu.app, folder.get());
                     },
         "s"},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(group), actions, G_N_ELEMENTS(actions), menu);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(directory.show_hidden.get()));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(directory.show_thumbnails.get()));
    gtk_widget_insert_action_group(desktop, "desktop", G_ACTION_GROUP(group));
    gtk_widget_insert_action_group(desktop, "sort", G_ACTION_GROUP(directory.sorting.actions.get()));

    auto *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(menu_pressed), menu);
    gtk_widget_add_controller(desktop, GTK_EVENT_CONTROLLER(click));

    auto *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(menu_key), menu);
    gtk_widget_add_controller(desktop, keys);
}
