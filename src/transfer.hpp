#pragma once

#include <gtk/gtk.h>

void transfer_attach(GtkWindow *window, GtkGridView *grid, GFile *desktop, GHashTable *staging);
