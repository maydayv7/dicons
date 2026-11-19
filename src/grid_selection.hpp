#pragma once

#include <gtk/gtk.h>
#include <vector>

// Borrowed from grid model until its contents change
std::vector<GFile *> selected_files(GtkSelectionModel *selection);
void select_when_listed(GtkGridView *grid, GFile *file, bool focus = false);
