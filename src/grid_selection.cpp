#include "grid_selection.hpp"
#include "desktop_directory.hpp"
#include "inline_rename.hpp"

std::vector<GFile *> selected_files(GtkSelectionModel *selection)
{
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
    std::vector<GFile *> files;
    GtkBitsetIter iter;
    guint position;
    for (gboolean valid = gtk_bitset_iter_init_first(&iter, selected, &position);
         valid; valid = gtk_bitset_iter_next(&iter, &position))
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(selection), position))};
        if (item_file(info.get()) && !is_trash_icon(info.get()))
            files.push_back(item_file(info.get()));
    }
    return files;
}

namespace
{
    struct PendingSelection
    {
        FileSet files = make_file_set();
        ObjectPtr<GFile> focus;
        ObjectPtr<GFile> rename;
    };
}

static void select_arrivals(GListModel *model, guint first, guint, guint added, GtkGridView *grid)
{
    auto *pending = static_cast<PendingSelection *>(g_object_get_data(G_OBJECT(grid), "pending-selection"));
    for (guint i = first; i < first + added && g_hash_table_size(pending->files.get()); ++i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(model, i))};
        GFile *file = item_file(info.get());
        if (!file || !g_hash_table_remove(pending->files.get(), file))
            continue;
        bool rename = pending->rename && g_file_equal(pending->rename.get(), file);
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(model), i, rename);
        if (rename && gtk_widget_get_root(GTK_WIDGET(grid)))
        {
            pending->rename.reset();
            start_inline_rename(grid, i);
        }
        if (pending->focus && g_file_equal(pending->focus.get(), file))
        {
            pending->focus.reset();
            auto *root = gtk_widget_get_root(GTK_WIDGET(grid));
            auto *focus = root ? gtk_root_get_focus(root) : nullptr;
            if (focus && !GTK_IS_EDITABLE(focus) && (focus == GTK_WIDGET(grid) || gtk_widget_is_ancestor(focus, GTK_WIDGET(grid))))
                gtk_grid_view_scroll_to(grid, i, GTK_LIST_SCROLL_FOCUS, nullptr);
        }
    }
}

void select_when_listed(GtkGridView *grid, GFile *file, ArrivalAction action)
{
    auto *model = G_LIST_MODEL(gtk_grid_view_get_model(grid));
    auto *pending = static_cast<PendingSelection *>(g_object_get_data(G_OBJECT(grid), "pending-selection"));
    if (!pending)
    {
        pending = new PendingSelection;
        g_object_set_data_full(G_OBJECT(grid), "pending-selection", pending, [](gpointer p)
                               { delete static_cast<PendingSelection *>(p); });
        g_signal_connect_object(model, "items-changed", G_CALLBACK(select_arrivals), grid, G_CONNECT_AFTER);
    }
    g_hash_table_add(pending->files.get(), g_object_ref(file));
    if (action == ArrivalAction::rename)
        pending->rename = retain(file);
    if (action == ArrivalAction::focus)
        pending->focus = retain(file);
    select_arrivals(model, 0, 0, g_list_model_get_n_items(model), grid);
}
