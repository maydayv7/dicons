#pragma once

#include "file_rename.hpp"
#include <sys/stat.h>

struct TrashedFile
{
    ObjectPtr<GFile> original, stored, metadata;
    DropIdentity parent;
    struct stat identity{}, record{};
};

bool move_to_trash(GFile *file, TrashedFile &entry, GError **error);
bool trash_entry_available(const TrashedFile &entry);
ObjectPtr<GFile> restore_from_trash(const TrashedFile &entry, GError **error);
