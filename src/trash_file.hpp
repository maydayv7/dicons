#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>
#include <sys/stat.h>

struct TrashedFile
{
    ObjectPtr<GFile> original, stored, metadata;
    struct stat identity{}, record{};
};

bool move_to_trash(GFile *file, TrashedFile &entry, GError **error);
ObjectPtr<GFile> restore_from_trash(const TrashedFile &entry, GError **error);
