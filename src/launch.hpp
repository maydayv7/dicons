#pragma once

#include <gtk/gtk.h>

void open_item(GtkGridView *grid, guint position, GtkApplication *app);
void open_terminal(GtkApplication *app, GFile *folder);
