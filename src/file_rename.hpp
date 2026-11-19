#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>

bool rename_no_replace(GFile *source, GFile *destination, GError **error);
[[nodiscard]] ObjectPtr<GFile> rename_desktop_item(GFile *source, const char *name, GError **error);
