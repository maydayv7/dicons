#pragma once

#include <gtk/gtk.h>

void attach_inline_rename(GtkWindow *window, GtkGridView *grid);
void start_inline_rename(GtkGridView *grid, guint position);
bool cancel_inline_rename(GtkGridView *grid);
