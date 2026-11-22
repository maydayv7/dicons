#include "file_rename.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <string_view>
#include <unistd.h>

DropIdentity::DropIdentity(GFileInfo *info)
{
    if (!info)
        return;
    if (auto *value = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE))
        id = value;
    created = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_CREATED);
    nanoseconds = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_CREATED_NSEC);
}

bool DropIdentity::matches(GFile *file, GFileQueryInfoFlags flags, GError **error) const
{
    ObjectPtr<GFileInfo> current{g_file_query_info(file, drop_identity_attributes, flags, nullptr, error)};
    if (!current)
        return false;
    if (!id.empty() && *this == DropIdentity(current.get()))
        return true;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The path now refers to a different item.");
    return false;
}

DropIdentity file_identity(GFile *file, GFileQueryInfoFlags flags)
{
    ObjectPtr<GFileInfo> info{g_file_query_info(file, drop_identity_attributes, flags, nullptr, nullptr)};
    return DropIdentity(info.get());
}

bool matches_drag_source(const DragIdentities &identities, GFile *source, GError **error)
{
    g_autofree char *uri = g_file_get_uri(source);
    auto found = identities.find(uri);
    if (found != identities.end())
        return found->second.matches(source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, error);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The drag selection changed.");
    return false;
}

bool rename_no_replace(GFile *source, GFile *destination, GError **error)
{
    if (!g_file_is_native(source) || !g_file_is_native(destination))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Use your file manager to move items outside the local filesystem.");
        return false;
    }
#ifdef __linux__
    if (renameat2(AT_FDCWD, g_file_peek_path(source), AT_FDCWD,
                  g_file_peek_path(destination), RENAME_NOREPLACE) == 0)
        return true;
    int code = errno;
    if (code == EXDEV)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Use your file manager to move items between filesystems.");
    else
        g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(code), g_strerror(code));
    return false;
#else
    return g_file_move(source, destination, GFileCopyFlags(G_FILE_COPY_NO_FALLBACK_FOR_MOVE | G_FILE_COPY_NOFOLLOW_SYMLINKS),
                       nullptr, nullptr, nullptr, error);
#endif
}

ObjectPtr<GFile> rename_into_folder(GFile *source, GFile *folder, const char *name, bool keep_both, GError **error)
{
    if (!keep_both)
    {
        ObjectPtr<GFile> destination{g_file_get_child(folder, name)};
        if (rename_no_replace(source, destination.get(), error))
            return destination;
        return nullptr;
    }

    std::filesystem::path filename{name};
    bool directory = g_file_query_file_type(source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) == G_FILE_TYPE_DIRECTORY;
    const auto stem = directory ? filename : filename.stem();
    const auto extension = directory ? std::filesystem::path{} : filename.extension();
    const long name_max = pathconf(g_file_peek_path(folder), _PC_NAME_MAX);
    for (guint n = 1;; ++n)
    {
        auto suffix = std::format(" ({})", n);
        std::string_view prefix = stem.native(), ext = extension.native();
        if (name_max > 0 && suffix.size() < size_t(name_max) && prefix.size() + suffix.size() + ext.size() > size_t(name_max))
        {
            if (ext.size() + suffix.size() >= size_t(name_max))
            {
                prefix = filename.native();
                ext = {};
            }
            size_t end = size_t(name_max) - suffix.size() - ext.size();
            if (g_utf8_validate(prefix.data(), prefix.size(), nullptr))
                while (end > 0 && (static_cast<unsigned char>(prefix[end]) & 0xc0) == 0x80)
                    --end;
            prefix = prefix.substr(0, end);
        }
        auto alternative = std::format("{}{}{}", prefix, suffix, ext);
        ObjectPtr<GFile> destination{g_file_get_child(folder, alternative.c_str())};
        g_autoptr(GError) attempt = nullptr;
        if (rename_no_replace(source, destination.get(), &attempt))
            return destination;
        if (!g_error_matches(attempt, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            g_propagate_error(error, g_steal_pointer(&attempt));
            return nullptr;
        }
    }
}

ObjectPtr<GFile> move_into_folder(GFile *source, GFile *folder, bool keep_both, GError **error)
{
    if (!g_file_is_native(source) || !g_file_is_native(folder))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Use your file manager to move items outside the local filesystem.");
        return nullptr;
    }
    if (g_file_equal(folder, source) || g_file_has_prefix(folder, source))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE, "Cannot move a folder inside itself.");
        return nullptr;
    }
    // A desktop symlink may lead back to the item's current directory
    ObjectPtr<GFile> parent{g_file_get_parent(source)};
    std::error_code ec;
    if (parent && std::filesystem::equivalent(g_file_peek_path(parent.get()), g_file_peek_path(folder), ec))
        return retain(source);
    g_autofree char *name = g_file_get_basename(source);
    return rename_into_folder(source, folder, name, keep_both, error);
}

ObjectPtr<GFile> rename_desktop_item(GFile *source, const char *name, GError **error)
{
    const std::string_view filename{name};
    if (filename.empty() || filename == "." || filename == ".." || filename.find('/') != filename.npos)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Enter a filename within the Desktop folder.");
        return nullptr;
    }

    ObjectPtr<GFile> parent{g_file_get_parent(source)};
    ObjectPtr<GFile> destination{g_file_get_child_for_display_name(parent.get(), name, error)};
    if (!destination || !rename_no_replace(source, destination.get(), error))
        return nullptr;
    return destination;
}
