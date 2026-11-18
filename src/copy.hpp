#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>

// Stage a complete item and publish
[[nodiscard]] ObjectPtr<GFile> desktop_copy_item(GFile *source, GFile *desktop, GFile *stage,
                                                 bool keep_both, GError **error);
