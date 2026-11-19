#include "icon_grid.hpp"
#include "grid_selection.hpp"
#include "desktop_directory.hpp"
#include "launch.hpp"

#include <gio/gdesktopappinfo.h>

static bool is_launcher(GFileInfo *info)
{
    return g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
           has_launcher_name(info);
}

static void load_launcher(GTask *task, gpointer source, gpointer, GCancellable *)
{
    GFile *file = item_file(G_FILE_INFO(source));
    GDesktopAppInfo *app = g_desktop_app_info_new_from_filename(g_file_peek_path(file));
    g_task_return_pointer(task, app, g_object_unref);
}

static void launcher_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GtkListItem) item = GTK_LIST_ITEM(data);
    g_autoptr(GAppInfo) app = static_cast<GAppInfo *>(g_task_propagate_pointer(G_TASK(result), nullptr));
    if (g_object_get_data(G_OBJECT(item), "launcher-load") != result)
        return;
    g_object_set_data(G_OBJECT(item), "launcher-load", nullptr);
    if (!app || gtk_list_item_get_item(item) != source)
        return;

    GtkWidget *box = gtk_list_item_get_child(item);
    GtkImage *image = GTK_IMAGE(gtk_widget_get_first_child(box));
    GtkLabel *label = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(image)));
    gtk_label_set_text(label, g_app_info_get_display_name(app));
    if (g_app_info_get_icon(app))
        gtk_image_set_from_gicon(image, g_app_info_get_icon(app));
}

static void refresh_item(GtkListItem *item)
{
    GFileInfo *info = static_cast<GFileInfo *>(gtk_list_item_get_item(item));
    GtkWidget *box = gtk_list_item_get_child(item);
    GtkImage *image = GTK_IMAGE(gtk_widget_get_first_child(box));
    GtkLabel *label = GTK_LABEL(gtk_widget_get_next_sibling(GTK_WIDGET(image)));
    gtk_label_set_text(label, g_file_info_get_display_name(info));
    if (g_file_info_get_icon(info))
        gtk_image_set_from_gicon(image, g_file_info_get_icon(info));
    else
        gtk_image_set_from_icon_name(image, "text-x-generic");

    if (is_launcher(info))
    {
        g_autoptr(GTask) task = g_task_new(info, nullptr, launcher_loaded, g_object_ref(item));
        g_object_set_data(G_OBJECT(item), "launcher-load", task);
        g_task_run_in_thread(task, load_launcher);
    }
}

static void bind_item(GtkListItemFactory *, GtkListItem *item, gpointer)
{
    refresh_item(item);
}

static void content_changed(GFileMonitor *, GFile *file, GFile *,
                            GFileMonitorEvent event, GtkListItem *item)
{
    if (event != G_FILE_MONITOR_EVENT_CHANGED && event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        return;
    GFileInfo *info = static_cast<GFileInfo *>(gtk_list_item_get_item(item));
    if (info && is_launcher(info) && g_file_equal(file, item_file(info)))
        refresh_item(item);
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

static void setup_item(GtkListItemFactory *, GtkListItem *item, GFileMonitor *changes)
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
    gtk_list_item_set_child(item, box);

    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);
    g_signal_connect_object(drag, "prepare", G_CALLBACK(prepare_drag), item, GConnectFlags{});
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
    if (changes)
        g_signal_connect_object(changes, "changed", G_CALLBACK(content_changed), item, GConnectFlags{});
}

GtkGridView *create_icon_grid(const DesktopDirectory &directory, GtkApplication *app)
{
    auto *factory = gtk_signal_list_item_factory_new();
    auto *changes = directory.changes.get();
    g_signal_connect_data(factory, "setup", G_CALLBACK(setup_item), changes ? g_object_ref(changes) : nullptr, [](gpointer data, GClosure *)
                          { if (data) g_object_unref(data); }, GConnectFlags{});
    g_signal_connect(factory, "bind", G_CALLBACK(bind_item), nullptr);
    auto *selection = gtk_multi_selection_new(g_object_ref(directory.items.get()));
    auto *grid = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(selection), factory));
    gtk_grid_view_set_max_columns(grid, 128);
    gtk_grid_view_set_enable_rubberband(grid, TRUE);
    g_signal_connect_object(grid, "activate", G_CALLBACK(open_item), app, GConnectFlags{});
    return grid;
}
