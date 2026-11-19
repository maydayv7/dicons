#include "copy_stage.hpp"
#include "notifications.hpp"

#include <cerrno>

ObjectPtr<GFile> make_copy_stage(GFile *desktop, GHashTable *staging, GApplication *app)
{
    g_autofree char *temp = g_build_filename(g_file_peek_path(desktop), ".dicons-XXXXXX", nullptr);
    if (!g_mkdtemp_full(temp, 0700))
    {
        notify_error(app, "Could not start desktop copy", g_strerror(errno));
        return nullptr;
    }
    ObjectPtr<GFile> stage{g_file_new_for_path(temp)};
    g_hash_table_add(staging, g_object_ref(stage.get()));
    return stage;
}

void remove_copy_stage(GFile *stage, GHashTable *staging, GApplication *app)
{
    g_autoptr(GError) error = nullptr;
    if (!g_file_delete(stage, nullptr, &error))
        notify_error(app, "Could not remove temporary folder", error->message);
    g_hash_table_remove(staging, stage);
}
