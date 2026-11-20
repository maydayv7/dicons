#pragma once

#include "gobject_ptr.hpp"
#include "desktop_sort.hpp"
#include "trash_icon.hpp"
#include <gtk/gtk.h>

using FileSet = std::unique_ptr<GHashTable, GlibDeleter<g_hash_table_unref>>;

[[nodiscard]] FileSet make_file_set();
GFile *item_file(GFileInfo *info);

struct DesktopDirectory
{
    DesktopDirectory(GFile *folder, GtkApplication *app, GSettings *preferences);

    ObjectPtr<GFile> folder;
    GListModel *entries() const;
    ObjectPtr<GSettings> preferences;
    DesktopSort sorting;
    FileSet staging;
    ObjectPtr<GSimpleAction> show_hidden;
    ObjectPtr<GAction> show_thumbnails;
    TrashIcon trash;
    ObjectPtr<GListModel> items;

private:
    class Updates;
    std::shared_ptr<Updates> updates_;
};
