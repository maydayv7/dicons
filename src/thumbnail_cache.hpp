#pragma once

#include "thumbnailer.hpp"
#include <string>

struct ThumbnailVersion
{
    std::string mime;
    std::string target_path;
    guint64 modified;
    guint32 nanoseconds;
    goffset size;

    explicit ThumbnailVersion(GFileInfo *info);
    bool operator==(const ThumbnailVersion &) const = default;
};

bool can_preview(GFileInfo *info);
ObjectPtr<GdkPixbuf> load_thumbnail(GFile *file, const ThumbnailVersion &version, GCancellable *cancel,
                                    bool changed_within_second = false);
