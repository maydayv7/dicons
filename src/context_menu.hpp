#pragma once

#include <gtk/gtk.h>

struct DesktopDirectory;
void attach_context_menu(GtkWindow *window, GtkGridView *grid, const DesktopDirectory &directory);
