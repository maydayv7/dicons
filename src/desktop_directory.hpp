#pragma once

#include "gobject_ptr.hpp"
#include "desktop_sort.hpp"
#include "desktop_item.hpp"
#include "trash_icon.hpp"
#include <gtk/gtk.h>

using FileSet = std::unique_ptr<GHashTable, GlibDeleter<g_hash_table_unref>>;

[[nodiscard]] FileSet make_file_set();
GFile *item_file(GFileInfo *info);
ObjectPtr<GFile> open_desktop_folder(GSettings *settings, GError **error);

struct DesktopDirectory
{
    DesktopDirectory(GFile *folder, GtkApplication *app, GSettings *preferences);

    ObjectPtr<GFile> folder;
    ObjectPtr<GSettings> preferences;
    DesktopSort sorting;
    FileSet staging;
    ObjectPtr<GSimpleAction> show_hidden;
    ObjectPtr<GAction> show_thumbnails;
    ObjectPtr<DiconsItemUpdates> item_updates{make_item_updates()};
    TrashIcon trash;
    ObjectPtr<GListModel> items;

private:
    class Updates;
    std::shared_ptr<Updates> updates_;
};
