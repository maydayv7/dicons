#pragma once

#include <gio/gio.h>

bool has_launcher_name(GFileInfo *info);
bool is_desktop_launcher(GFileInfo *info);
void read_launcher_presentation(GFileInfo *info, GFile *file);
const char *item_label(GFileInfo *info);
void cache_filename_key(GFileInfo *info);
const char *filename_key(GFileInfo *info);
