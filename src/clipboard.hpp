#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>
#include <vector>

bool clipboard_has_files(GdkClipboard *clipboard);
bool clipboard_item_is_cut(GdkClipboard *clipboard, GFileInfo *info);
void copy_selection(GtkGridView *grid, bool cut);
void paste_selection(GtkGridView *grid, bool into_folder);

GdkContentProvider *file_clipboard_content(const std::vector<GFile *> &files, bool cut);
std::vector<ObjectPtr<GFile>> decode_file_clipboard(const char *text, bool has_operation, bool &cut, GError **error);
