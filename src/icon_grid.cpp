#include "icon_grid.hpp"
#include "grid_selection.hpp"
#include "desktop_directory.hpp"
#include "launch.hpp"
#include "desktop_item.hpp"

static void refresh_item(GtkListItem *item)
{
    GFileInfo *info = static_cast<GFileInfo *>(gtk_list_item_get_item(item));
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkImage *image = GTK_IMAGE(gtk_widget_get_first_child(box));
    GtkLabel *label = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(image)));
    gtk_label_set_text(label, item_label(info));
    if (g_file_info_get_icon(info))
        gtk_image_set_from_gicon(image, g_file_info_get_icon(info));
    else
        gtk_image_set_from_icon_name(image, "text-x-generic");
}

static void bind_item(GtkListItemFactory *, GtkListItem *item, gpointer)
{
    refresh_item(item);
}

static void entry_updated(GListModel *entries, guint position, guint removed, guint added, GtkListItem *item)
{
    if (removed == 1 && added == 1)
    {
        ObjectPtr<GFileInfo> changed{G_FILE_INFO(g_list_model_get_item(entries, position))};
        if (changed.get() == gtk_list_item_get_item(item))
            refresh_item(item);
    }
}

static GdkContentProvider *prepare_drag(GtkDragSource *, double, double,
                                        GtkListItem *item)
{
    GtkWidget *tile = gtk_list_item_get_child(item);
    GtkGridView *grid = GTK_GRID_VIEW(gtk_widget_get_ancestor(tile, GTK_TYPE_GRID_VIEW));
    GtkSelectionModel *selection = gtk_grid_view_get_model(grid);
    guint position = gtk_list_item_get_position(item);
    if (position == GTK_INVALID_LIST_POSITION)
        return nullptr;
    if (!gtk_selection_model_is_selected(selection, position))
        gtk_selection_model_select_item(selection, position, TRUE);

    auto files = selected_files(selection);
    g_auto(GValue) value = G_VALUE_INIT;
    g_value_init(&value, GDK_TYPE_FILE_LIST);
    g_value_take_boxed(&value, gdk_file_list_new_from_array(files.data(), files.size()));
    return gdk_content_provider_new_for_value(&value);
}

static void setup_item(GtkListItemFactory *, GtkListItem *item, GListModel *entries)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *image = gtk_image_new();
    GtkWidget *label = gtk_label_new(nullptr);
    gtk_widget_set_size_request(box, 96, -1);
    gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(label), 2);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 12);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    g_object_set_data(G_OBJECT(box), "desktop-item", item);
    gtk_list_item_set_child(item, box);

    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);
    g_signal_connect_object(drag, "prepare", G_CALLBACK(prepare_drag), item, GConnectFlags{});
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
    g_signal_connect_object(entries, "items-changed", G_CALLBACK(entry_updated), item, GConnectFlags{});
}

GtkGridView *create_icon_grid(const DesktopDirectory &directory, GtkApplication *app)
{
    auto *factory = gtk_signal_list_item_factory_new();
    g_signal_connect_object(factory, "setup", G_CALLBACK(setup_item), directory.entries(), GConnectFlags{});
    g_signal_connect(factory, "bind", G_CALLBACK(bind_item), nullptr);
    auto *selection = gtk_multi_selection_new(g_object_ref(directory.items.get()));
    auto *grid = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(selection), factory));
    gtk_grid_view_set_max_columns(grid, 128);
    gtk_grid_view_set_enable_rubberband(grid, TRUE);
    g_signal_connect_object(grid, "activate", G_CALLBACK(open_item), app, GConnectFlags{});
    return grid;
}
