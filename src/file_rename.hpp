#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>
#include <string>
#include <unordered_map>

inline constexpr char drop_identity_attributes[] = "id::file,time::created,time::created-nsec";

struct DropIdentity
{
    explicit DropIdentity(GFileInfo *info = nullptr);
    bool matches(GFile *file, GFileQueryInfoFlags flags, GError **error) const;
    bool operator==(const DropIdentity &) const = default;

    std::string id;
    guint64 created{};
    guint32 nanoseconds{};
};

DropIdentity file_identity(GFile *file, GFileQueryInfoFlags flags = G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS);

using DragIdentities = std::unordered_map<std::string, DropIdentity>;
bool matches_drag_source(const DragIdentities &identities, GFile *source, GError **error);

bool move_to_path(GFile *source, GFile *destination, GError **error);
bool rename_no_replace(GFile *source, GFile *destination, GError **error);
[[nodiscard]] ObjectPtr<GFile> move_into_folder(GFile *source, GFile *folder, bool keep_both, GError **error);
[[nodiscard]] ObjectPtr<GFile> publish_copy(GFile *staged, GFile *folder, const char *name, bool keep_both, GError **error);
[[nodiscard]] ObjectPtr<GFile> rename_desktop_item(GFile *source, const char *name, GError **error);
