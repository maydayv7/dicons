#pragma once

#include "file_rename.hpp"
#include <gtk/gtk.h>
#include <functional>
#include <vector>

void transfer_attach(GtkWindow *window, GtkGridView *grid, GFile *desktop, GHashTable *staging);

using PasteFinished = std::function<void(const std::vector<GFile *> &remaining)>;
void transfer_pasted_files(GtkGridView *grid, GFile *folder, const DropIdentity &target, const std::vector<GFile *> &files,
                           bool move, std::shared_ptr<const DragIdentities> identities, PasteFinished finished);
