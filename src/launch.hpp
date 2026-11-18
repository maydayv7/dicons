#pragma once

#include <gtk/gtk.h>

bool has_launcher_name(GFileInfo *info);
void open_item(GtkGridView *grid, guint position, GtkApplication *app);
