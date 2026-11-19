#include "selection_actions.hpp"
#include "desktop_directory.hpp"
#include "grid_selection.hpp"
#include "inline_rename.hpp"
#include "notifications.hpp"

static GHashTable *pending_trash(GApplication *app)
{
    auto *pending = static_cast<GHashTable *>(g_object_get_data(G_OBJECT(app), "pending-trash"));
    if (!pending)
    {
        pending = make_file_set().release();
        g_object_set_data_full(G_OBJECT(app), "pending-trash", pending, [](gpointer p)
                               { g_hash_table_unref(static_cast<GHashTable *>(p)); });
    }
    return pending;
}

static void trash_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    ObjectPtr<GApplication> app{G_APPLICATION(data)};
    GFile *file = G_FILE(source);
    g_hash_table_remove(pending_trash(app.get()), file);
    g_autoptr(GError) error = nullptr;
    if (!g_file_trash_finish(file, result, &error))
        notify_file_error(app.get(), "Could not move item to Trash", file, error);
}

void trash_selection(GtkGridView *grid)
{
    auto *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
    auto *app = G_APPLICATION(gtk_window_get_application(window));
    auto *pending = pending_trash(app);
    for (GFile *file : selected_files(gtk_grid_view_get_model(grid)))
        if (g_hash_table_add(pending, g_object_ref(file)))
            g_file_trash_async(file, G_PRIORITY_DEFAULT, nullptr, trash_finished, g_object_ref(app));
}

void rename_selection(GtkGridView *grid)
{
    g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(gtk_grid_view_get_model(grid));
    if (gtk_bitset_get_size(selected) == 1)
        start_inline_rename(grid, gtk_bitset_get_minimum(selected));
}
