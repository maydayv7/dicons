#include "undo.hpp"
#include "icon_grid.hpp"
#include "grid_layout.hpp"
#include "grid_selection.hpp"
#include "desktop_directory.hpp"
#include "launch.hpp"
#include "desktop_item.hpp"
#include "item_icon.hpp"

#include <algorithm>
#include <unordered_map>

namespace
{
    struct Grid
    {
        GridLayout layout;
        ObjectPtr<GAction> thumbnails;
        int cell_height{};
        guint resize_tick{};
        std::unordered_map<GFileInfo *, GtkListItem *> bound;
    };

    Grid &state(GtkGridView *grid)
    {
        return *static_cast<Grid *>(g_object_get_data(G_OBJECT(gtk_grid_view_get_factory(grid)), "grid"));
    }

    int row_height(GtkWidget *widget, int size)
    {
        g_autoptr(PangoLayout) lines = gtk_widget_create_pango_layout(widget, "Ag\nAg");
        int height;
        pango_layout_get_pixel_size(lines, nullptr, &height);
        // Allow label lines, icon-to-label gap and vertical padding
        return size + height + 10;
    }
}

GtkListItem *bound_item(GtkGridView *grid, GFileInfo *info)
{
    auto &items = state(grid).bound;
    auto found = items.find(info);
    return found == items.end() ? nullptr : found->second;
}

GtkWidget *tile_content(GtkListItem *item)
{
    return gtk_widget_get_first_child(gtk_list_item_get_child(item));
}

static GtkListItem *target_slot(GtkWidget *widget, GtkGridView *grid)
{
    while (widget && gtk_widget_get_parent(widget) != GTK_WIDGET(grid))
        widget = gtk_widget_get_parent(widget);
    auto *cell = widget ? gtk_widget_get_first_child(widget) : nullptr;
    return cell ? static_cast<GtkListItem *>(g_object_get_data(G_OBJECT(cell), "desktop-item")) : nullptr;
}

GtkListItem *target_item(GtkWidget *widget, GtkGridView *grid)
{
    auto *item = target_slot(widget, grid);
    return item && gtk_list_item_get_selectable(item) ? item : nullptr;
}

static void refresh_item(GtkListItem *item)
{
    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    GtkWidget *icon = gtk_widget_get_first_child(tile_content(item));
    GtkLabel *label = GTK_LABEL(gtk_widget_get_next_sibling(icon));
    gtk_label_set_text(label, item_label(info));
    bind_item_icon(icon, info);
}

static void bind_item(GtkListItemFactory *, GtkListItem *item, Grid *grid)
{
    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    bool occupied = item_file(info) != nullptr;
    gtk_list_item_set_selectable(item, occupied);
    gtk_list_item_set_activatable(item, occupied);
    gtk_list_item_set_focusable(item, occupied);
    gtk_widget_set_visible(tile_content(item), occupied);
    if (occupied)
    {
        grid->bound.emplace(info, item);
        refresh_item(item);
    }
}

static void unbind_item(GtkListItemFactory *, GtkListItem *item, Grid *grid)
{
    grid->bound.erase(G_FILE_INFO(gtk_list_item_get_item(item)));
    unbind_item_icon(gtk_widget_get_first_child(tile_content(item)));
}

static void entry_updated(GListModel *entries, guint position, guint removed, guint added, GtkGridView *grid)
{
    if (removed == 1 && added == 1)
    {
        ObjectPtr<GFileInfo> changed{G_FILE_INFO(g_list_model_get_item(entries, position))};
        if (auto *item = bound_item(grid, changed.get()))
            refresh_item(item);
    }
}

struct DesktopDrag
{
    ObjectPtr<GtkGridView> origin;
    std::vector<std::string> uris;
    bool trash;
    std::shared_ptr<DragIdentities> identities = std::make_shared<DragIdentities>();
};

static DesktopDrag *desktop_drag(GdkDrop *drop)
{
    auto *drag = gdk_drop_get_drag(drop);
    return drag ? static_cast<DesktopDrag *>(g_object_get_data(G_OBJECT(drag), "desktop-drag")) : nullptr;
}

GtkGridView *drag_origin(GdkDrop *drop)
{
    auto *drag = desktop_drag(drop);
    return drag ? drag->origin.get() : nullptr;
}

bool dragging_trash(GdkDrop *drop)
{
    auto *drag = desktop_drag(drop);
    return drag && drag->trash;
}

std::shared_ptr<const DragIdentities> drag_identities(GdkDrop *drop)
{
    auto *drag = desktop_drag(drop);
    return drag ? drag->identities : nullptr;
}

bool move_icons(GdkDrop *drop, GtkGridView *grid, GtkWidget *window, double x, double y)
{
    auto *drag = desktop_drag(drop);
    auto &view = state(grid);
    if (!drag)
        return false;
    auto *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
    auto move = [&](guint position)
    {
        bool moved = false;
        change_view(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(window))), view.layout.settings.get(), nullptr,
                    [&]
                    { moved = view.layout.move(drag->uris, position); });
        return moved;
    };
    if (auto *slot = target_slot(picked, grid))
        return move(gtk_list_item_get_position(slot));

    // GTK has no list item below the final row
    graphene_point_t from{float(x), float(y)}, point;
    if (!gtk_widget_compute_point(window, GTK_WIDGET(grid), &from, &point) || !view.cell_height)
        return false;
    auto &layout = view.layout;
    int width = gtk_widget_get_width(GTK_WIDGET(grid));
    int pitch = std::max(1, width / int(layout.columns));
    int column = std::clamp(int(point.x) / pitch, 0, int(layout.columns) - 1);
    if (gtk_widget_get_direction(GTK_WIDGET(grid)) == GTK_TEXT_DIR_RTL)
        column = layout.columns - 1 - column;
    auto *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(grid));
    guint count = g_list_model_get_n_items(G_LIST_MODEL(layout.slots.get()));
    guint rows = std::max(layout.rows, (count + layout.columns - 1) / layout.columns);
    int row = std::clamp(int(point.y + gtk_adjustment_get_value(vertical)) / view.cell_height, 0, int(rows) - 1);
    return move(row * layout.columns + column);
}

static GdkContentProvider *prepare_drag(GtkDragSource *, double, double, GtkListItem *item)
{
    auto *tile = tile_content(item);
    auto *grid = GTK_GRID_VIEW(gtk_widget_get_ancestor(tile, GTK_TYPE_GRID_VIEW));
    auto *selection = gtk_grid_view_get_model(grid);
    guint position = gtk_list_item_get_position(item);
    if (position == GTK_INVALID_LIST_POSITION || !gtk_list_item_get_selectable(item))
        return nullptr;
    if (!gtk_selection_model_is_selected(selection, position))
        gtk_selection_model_select_item(selection, position, TRUE);

    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    if (is_trash_icon(info))
        return gdk_content_provider_new_typed(G_TYPE_FILE_INFO, info);
    return selected_file_content(selection);
}

static void drag_began(GtkDragSource *, GdkDrag *gdk_drag, GtkListItem *item)
{
    auto *grid = GTK_GRID_VIEW(gtk_widget_get_ancestor(tile_content(item), GTK_TYPE_GRID_VIEW));
    auto *info = G_FILE_INFO(gtk_list_item_get_item(item));
    auto *selection = gtk_grid_view_get_model(grid);
    auto *drag = new DesktopDrag{retain(grid), {}, is_trash_icon(info)};
    g_object_set_data_full(G_OBJECT(gdk_drag), "desktop-drag", drag, [](gpointer p)
                           { delete static_cast<DesktopDrag *>(p); });
    guint position = gtk_list_item_get_position(item);
    g_autofree char *anchor = g_file_get_uri(item_file(info));
    drag->uris.emplace_back(anchor);
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
    GtkBitsetIter iter;
    guint i;
    for (bool valid = gtk_bitset_iter_init_first(&iter, selected, &i); valid; valid = gtk_bitset_iter_next(&iter, &i))
    {
        ObjectPtr<GFileInfo> other{G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(selection), i))};
        if (auto *file = item_file(other.get()))
        {
            g_autofree char *uri = g_file_get_uri(file);
            if (i != position)
                drag->uris.emplace_back(uri);
            if (!is_trash_icon(other.get()))
                drag->identities->emplace(uri, listed_identity(other.get(), file));
        }
    }
}

static void cell_size_changed(GSettings *settings, const char *, GtkWidget *cell)
{
    gtk_widget_set_size_request(cell, -1, row_height(cell, g_settings_get_int(settings, "icon-size")));
}

static void setup_item(GtkListItemFactory *, GtkListItem *item, Grid *grid)
{
    auto *settings = grid->layout.settings.get();
    GtkWidget *cell = gtk_overlay_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *image = create_item_icon(grid->thumbnails.get(), settings);
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_widget_add_css_class(cell, "desktop-cell");
    gtk_widget_add_css_class(box, "desktop-tile");
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(label), 2);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 12);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    gtk_widget_set_valign(box, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(cell), box);
    g_object_set_data(G_OBJECT(cell), "desktop-item", item);
    gtk_list_item_set_child(item, cell);
    cell_size_changed(settings, nullptr, cell);
    g_signal_connect_object(settings, "changed::icon-size", G_CALLBACK(cell_size_changed), cell, GConnectFlags{});

    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);
    g_signal_connect_object(drag, "prepare", G_CALLBACK(prepare_drag), item, GConnectFlags{});
    g_signal_connect_object(drag, "drag-begin", G_CALLBACK(drag_began), item, GConnectFlags{});
    gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(drag));
}

static void arrange(GtkGridView *grid)
{
    auto &layout = state(grid).layout;
    auto *scroll = GTK_SCROLLABLE(grid);
    auto *horizontal = gtk_scrollable_get_hadjustment(scroll);
    auto *vertical = gtk_scrollable_get_vadjustment(scroll);
    int size = g_settings_get_int(layout.settings.get(), "icon-size");
    int width = horizontal ? gtk_adjustment_get_page_size(horizontal) : 0;
    int height = vertical ? gtk_adjustment_get_page_size(vertical) : 0;
    int cell_width = std::max(64, size + 24);
    guint columns = std::clamp(width / cell_width, 1, 128);
    state(grid).cell_height = row_height(GTK_WIDGET(grid), size);
    guint rows = std::max(1, height / state(grid).cell_height);
    gtk_grid_view_set_max_columns(grid, columns);
    gtk_grid_view_set_min_columns(grid, columns);
    layout.resize(columns, rows);
}

static gboolean viewport_resized(GtkWidget *widget, GdkFrameClock *, gpointer)
{
    auto *grid = GTK_GRID_VIEW(widget);
    state(grid).resize_tick = 0;
    arrange(grid);
    return G_SOURCE_REMOVE;
}

static void viewport_changed(GtkAdjustment *, GParamSpec *, GtkGridView *grid)
{
    auto &view = state(grid);
    if (!view.resize_tick)
        view.resize_tick = gtk_widget_add_tick_callback(GTK_WIDGET(grid), viewport_resized, nullptr, nullptr);
}

static void grid_pressed(GtkGestureClick *click, int, double x, double y, GtkGridView *grid)
{
    auto *surface = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
    auto *picked = gtk_widget_pick(surface, x, y, GTK_PICK_DEFAULT);
    gtk_grid_view_set_enable_rubberband(grid, target_item(picked, grid) == nullptr);
}

void attach_grid_viewport(GtkScrolledWindow *viewport, GtkGridView *grid)
{
    gtk_scrolled_window_set_child(viewport, GTK_WIDGET(grid));
    for (auto *adjustment : {gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(grid)),
                             gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(grid))})
        g_signal_connect_object(adjustment, "notify::page-size", G_CALLBACK(viewport_changed), grid, GConnectFlags{});
    arrange(grid);

    auto *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect_object(click, "pressed", G_CALLBACK(grid_pressed), grid, GConnectFlags{});
    gtk_widget_add_controller(GTK_WIDGET(viewport), GTK_EVENT_CONTROLLER(click));
}

static void exclude_empty_cells(GtkSelectionModel *selection, guint first, guint count, gpointer)
{
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection_in_range(selection, first, count);
    g_autoptr(GtkBitset) holes = gtk_bitset_new_empty();
    GtkBitsetIter iter;
    guint i;
    for (bool valid = gtk_bitset_iter_init_first(&iter, selected, &i); valid; valid = gtk_bitset_iter_next(&iter, &i))
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(selection), i))};
        if (!item_file(info.get()))
            gtk_bitset_add(holes, i);
    }
    if (!gtk_bitset_is_empty(holes))
    {
        g_autoptr(GtkBitset) empty = gtk_bitset_new_empty();
        gtk_selection_model_set_selection(selection, empty, holes);
    }
}

GtkGridView *create_icon_grid(const DesktopDirectory &directory, GtkApplication *app)
{
    auto *factory = gtk_signal_list_item_factory_new();
    auto *data = new Grid{GridLayout(directory.items.get(), directory.preferences.get()), retain(directory.show_thumbnails.get())};
    g_object_set_data_full(G_OBJECT(factory), "grid", data, [](gpointer p)
                           { delete static_cast<Grid *>(p); });
    g_signal_connect(factory, "setup", G_CALLBACK(setup_item), data);
    g_signal_connect(factory, "bind", G_CALLBACK(bind_item), data);
    g_signal_connect(factory, "unbind", G_CALLBACK(unbind_item), data);
    auto *selection = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(data->layout.slots.get())));
    g_signal_connect(selection, "selection-changed", G_CALLBACK(exclude_empty_cells), nullptr);
    auto *grid = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(selection), factory));
    g_signal_connect_object(directory.preferences.get(), "changed::icon-size", G_CALLBACK(+[](GSettings *, const char *, GtkGridView *grid)
                                                                                          { arrange(grid); }),
                            grid, GConnectFlags{});
    g_signal_connect_object(directory.entries(), "items-changed", G_CALLBACK(entry_updated), grid, GConnectFlags{});
    g_signal_connect_object(directory.trash.items.get(), "items-changed", G_CALLBACK(entry_updated), grid, GConnectFlags{});
    gtk_grid_view_set_enable_rubberband(grid, TRUE);
    g_signal_connect_object(grid, "activate", G_CALLBACK(open_item), app, GConnectFlags{});
    return grid;
}
