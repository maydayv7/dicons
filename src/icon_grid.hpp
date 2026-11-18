#pragma once

#include <gtk/gtk.h>

struct DesktopDirectory;
GtkGridView *create_icon_grid(const DesktopDirectory &directory, GtkApplication *app);
