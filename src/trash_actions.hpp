#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <vector>

void trash_files(GtkGridView *origin, const std::vector<GFile *> &files,
                 std::function<void(bool)> finished = {});
void undo_trash(GtkGridView *grid);
