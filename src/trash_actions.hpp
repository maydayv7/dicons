#pragma once

#include "file_rename.hpp"
#include <gtk/gtk.h>
#include <functional>
#include <vector>

void trash_files(GtkGridView *origin, const std::vector<GFile *> &files,
                 std::function<void(bool)> finished = {}, const DragIdentities *identities = nullptr);
