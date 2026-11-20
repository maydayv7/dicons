#pragma once

#include <gtk/gtk.h>

struct DesktopDirectory;
GtkGridView *create_icon_grid(const DesktopDirectory &directory, GtkApplication *app);
GtkListItem *bound_item(GtkGridView *grid, GFileInfo *info);
GtkListItem *target_item(GtkWidget *widget, GtkGridView *grid);
GtkWidget *tile_content(GtkListItem *item);
void attach_grid_viewport(GtkScrolledWindow *viewport, GtkGridView *grid);
GtkGridView *drag_origin(GdkDrop *drop);
bool dragging_trash(GdkDrop *drop);
bool move_icons(GdkDrop *drop, GtkGridView *grid, GtkWidget *window, double x, double y);
