#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>

struct DiconsItemUpdates;
ObjectPtr<DiconsItemUpdates> make_item_updates();
void notify_item_updated(DiconsItemUpdates *updates, GFileInfo *info);

bool has_launcher_name(GFileInfo *info);
bool is_desktop_launcher(GFileInfo *info);
void read_launcher_presentation(GFileInfo *info, GFile *file);
const char *item_label(GFileInfo *info);
void cache_filename_key(GFileInfo *info);
const char *filename_key(GFileInfo *info);
