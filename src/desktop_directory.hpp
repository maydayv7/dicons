#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>

using FileSet = std::unique_ptr<GHashTable, GlibDeleter<g_hash_table_unref>>;

[[nodiscard]] FileSet make_file_set();
GFile *item_file(GFileInfo *info);

struct DesktopDirectory
{
    DesktopDirectory(GFile *folder, GtkApplication *app);

    ObjectPtr<GFile> folder;
    ObjectPtr<GFileMonitor> changes;
    FileSet staging;
    ObjectPtr<GListModel> items;

private:
    class Updates;
    std::shared_ptr<Updates> updates_;
};
