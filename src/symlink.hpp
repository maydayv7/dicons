#pragma once

#include <gio/gio.h>
#include <functional>

void read_symlink_target(GFileInfo *info, GFile *file);
const char *symlink_target_path(GFileInfo *info);
void watch_symlink_target(GFileInfo *item, std::function<void()> refresh);
