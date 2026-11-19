#include "desktop_windows.hpp"
#include "desktop_directory.hpp"
#include "icon_grid.hpp"
#include "keyboard.hpp"
#include "context_menu.hpp"
#include "transfer.hpp"

#include <gtk4-layer-shell.h>
#include <algorithm>
#include <vector>

// Keep a transparent pixel so Wayland accepts first drop
static GtkWidget *desktop_overlay(GtkWidget *scroll)
{
    static const guint8 pixel[] = {0, 0, 0, 0};
    g_autoptr(GBytes) bytes = g_bytes_new_static(pixel, sizeof(pixel));
    g_autoptr(GdkTexture) texture = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, bytes, 4);
    GtkWidget *anchor = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
    gtk_widget_set_halign(anchor, GTK_ALIGN_START);
    gtk_widget_set_valign(anchor, GTK_ALIGN_START);
    gtk_widget_set_can_target(anchor, FALSE);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), anchor);
    return overlay;
}

static void add_window(GtkApplication *app, const DesktopDirectory &directory, GdkMonitor *monitor)
{
    GtkWindow *window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(window, "Desktop Icons");
    gtk_widget_add_css_class(GTK_WIDGET(window), "dicons");
    gtk_layer_init_for_window(window);
    gtk_layer_set_monitor(window, monitor);
    gtk_layer_set_namespace(window, "dicons");
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_BOTTOM);
    for (auto edge : {GTK_LAYER_SHELL_EDGE_LEFT, GTK_LAYER_SHELL_EDGE_RIGHT,
                      GTK_LAYER_SHELL_EDGE_TOP, GTK_LAYER_SHELL_EDGE_BOTTOM})
        gtk_layer_set_anchor(window, edge, TRUE);
    gtk_layer_set_margin(window, GTK_LAYER_SHELL_EDGE_TOP, 20);
    if (gtk_layer_get_protocol_version() >= 4)
        gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
    g_object_set_data_full(G_OBJECT(window), "output", g_object_ref(monitor), g_object_unref);

    auto *grid = create_icon_grid(directory, app);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(grid));
    gtk_window_set_child(window, desktop_overlay(scroll));
    attach_desktop_keys(window, grid);
    attach_context_menu(window, grid, directory);
    transfer_attach(window, grid, directory.folder.get(), directory.staging.get());
    gtk_window_present(window);
}

void sync_windows(GtkApplication *app, GListModel *monitors, const DesktopDirectory &directory)
{
    std::vector<ObjectPtr<GdkMonitor>> missing;
    for (guint i = 0; i < g_list_model_get_n_items(monitors); ++i)
        missing.emplace_back(GDK_MONITOR(g_list_model_get_item(monitors, i)));

    // Destroying a window changes GTK's window list
    std::vector<ObjectPtr<GtkWindow>> obsolete;
    for (auto *link = gtk_application_get_windows(app); link; link = link->next)
    {
        auto *window = GTK_WINDOW(link->data);
        auto *output = static_cast<GdkMonitor *>(g_object_get_data(G_OBJECT(window), "output"));
        if (!output)
            continue;
        const auto it = std::ranges::find(missing, output, &ObjectPtr<GdkMonitor>::get);
        if (it == missing.end())
            obsolete.push_back(retain(window));
        else
            missing.erase(it);
    }
    for (const auto &window : obsolete)
        gtk_window_destroy(window.get());
    for (const auto &output : missing)
        add_window(app, directory, output.get());
}

void install_desktop_style(GdkDisplay *display)
{
    g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
    const char *style = "window.dicons, window.dicons scrolledwindow, window.dicons gridview {"
                        " background: transparent; }";
    gtk_css_provider_load_from_string(css, style);
    // Desktop surfaces must stay transparent
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
}
