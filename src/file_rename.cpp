#include "file_rename.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <string_view>
#include <mutex>
#include <sys/stat.h>
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

static bool errno_error(int code, GError **error)
{
    g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(code), g_strerror(code));
    return false;
}

static bool same_version(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

static bool move_across_devices(GFile *source, GFile *destination, GError **error)
{
    const char *from = g_file_peek_path(source);
    const char *to = g_file_peek_path(destination);
    struct stat original{}, current{};
    if (lstat(from, &original) < 0)
        return errno_error(errno, error);
    if (!S_ISREG(original.st_mode) && !S_ISLNK(original.st_mode))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Use your file manager to move folders or special files between filesystems.");
        return false;
    }
    if (lstat(to, &current) == 0)
        return errno_error(EEXIST, error);
    if (errno != ENOENT)
        return errno_error(errno, error);
    ObjectPtr<GFile> folder{g_file_get_parent(destination)};
    g_autofree char *stage = g_build_filename(g_file_peek_path(folder.get()), ".dicons-XXXXXX", nullptr);
    if (!g_mkdtemp_full(stage, 0700))
        return errno_error(errno, error);
    ObjectPtr<GFile> staged{g_file_new_build_filename(stage, "item", nullptr)};
    const auto flags = GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA);
    bool moved = false;
    if (g_file_copy(source, staged.get(), flags, nullptr, nullptr, nullptr, error))
    {
        static std::mutex commit;
        const std::lock_guard lock(commit);
        if (lstat(from, &current) < 0)
            errno_error(errno, error);
        else if (!same_version(original, current))
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The source changed while it was being copied.");
        else if (rename_no_replace(staged.get(), destination, error))
        {
            struct stat published{};
            bool observed = lstat(to, &published) == 0;
            if (unlink(from) == 0)
                moved = true;
            else
            {
                errno_error(errno, error);
                if (observed && lstat(to, &current) == 0 && same_version(published, current))
                    unlink(to);
            }
        }
    }
    unlink(g_file_peek_path(staged.get()));
    rmdir(stage);
    return moved;
}

bool rename_no_replace(GFile *source, GFile *destination, GError **error, bool allow_file_copy)
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
    if (code == EXDEV && allow_file_copy)
        return move_across_devices(source, destination, error);
    return errno_error(code, error);
#else
    return g_file_move(source, destination, GFileCopyFlags(G_FILE_COPY_NO_FALLBACK_FOR_MOVE | G_FILE_COPY_NOFOLLOW_SYMLINKS),
                       nullptr, nullptr, nullptr, error);
#endif
}

ObjectPtr<GFile> rename_into_folder(GFile *source, GFile *folder, const char *name, bool keep_both, GError **error, bool allow_file_copy)
{
    if (!keep_both)
    {
        ObjectPtr<GFile> destination{g_file_get_child(folder, name)};
        if (rename_no_replace(source, destination.get(), error, allow_file_copy))
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
        if (rename_no_replace(source, destination.get(), &attempt, allow_file_copy))
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
    return rename_into_folder(source, folder, name, keep_both, error, true);
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
