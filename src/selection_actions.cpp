#include "selection_actions.hpp"
#include "desktop_directory.hpp"
#include "grid_selection.hpp"
#include "inline_rename.hpp"
#include "trash_actions.hpp"

void trash_selection(GtkGridView *grid)
{
    trash_files(grid, selected_files(gtk_grid_view_get_model(grid)));
}

void rename_selection(GtkGridView *grid)
{
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(gtk_grid_view_get_model(grid));
    if (gtk_bitset_get_size(selected) == 1)
        start_inline_rename(grid, gtk_bitset_get_minimum(selected));
}
