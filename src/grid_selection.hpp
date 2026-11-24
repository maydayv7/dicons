#pragma once
#include "file_rename.hpp"

#include <gtk/gtk.h>
#include <vector>

// Borrowed from grid model until its contents change
std::vector<GFile *> selected_files(GtkSelectionModel *selection, DragIdentities *identities = nullptr);
GdkContentProvider *selected_file_content(GtkSelectionModel *selection);
enum class ArrivalAction
{
    select,
    focus,
    rename
};
void select_when_listed(GtkGridView *grid, GFile *file, ArrivalAction action = ArrivalAction::select);
