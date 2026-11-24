#pragma once

#include <gtk/gtk.h>
#include "file_rename.hpp"

struct DesktopDirectory;
class GridLayout;
GtkGridView *create_icon_grid(const DesktopDirectory &directory, GtkApplication *app);
GtkListItem *bound_item(GtkGridView *grid, GFileInfo *info);
GtkListItem *target_item(GtkWidget *widget, GtkGridView *grid);
guint grid_position_at(GtkGridView *grid, double x, double y);
GridLayout &grid_layout(GtkGridView *grid);
void highlight_drop_target(GtkGridView *grid, GFileInfo *info);
GtkWidget *tile_content(GtkListItem *item);
void attach_grid_viewport(GtkScrolledWindow *viewport, GtkGridView *grid);
GtkGridView *drag_origin(GdkDrop *drop);
bool dragging_trash(GdkDrop *drop);
std::shared_ptr<const DragIdentities> drag_identities(GdkDrop *drop);
bool move_icons(GdkDrop *drop, GtkGridView *grid, guint position);
