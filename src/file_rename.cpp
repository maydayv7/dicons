#include "file_rename.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <string_view>
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

static constexpr char link_identity[] = "dicons::link-identity";

void cache_link_identity(GFileInfo *info, GFile *file)
{
    if (g_file_info_get_is_symlink(info))
    {
        ObjectPtr<GFileInfo> link{g_file_query_info(file, drop_identity_attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr)};
        if (link && g_file_info_get_is_symlink(link.get()))
            g_file_info_set_attribute_object(info, link_identity, G_OBJECT(link.get()));
    }
}

DropIdentity listed_identity(GFileInfo *info, GFile *file)
{
    if (!g_file_info_get_is_symlink(info))
        return DropIdentity(info);
    // Initial directory listings are visible before their background link refresh
    if (!g_file_info_has_attribute(info, link_identity))
        cache_link_identity(info, file);
    return DropIdentity(G_FILE_INFO(g_file_info_get_attribute_object(info, link_identity)));
}

const DropIdentity *verify_drag_source(const DragIdentities &identities, GFile *source, GError **error)
{
    g_autofree char *uri = g_file_get_uri(source);
    auto found = identities.find(uri);
    if (found != identities.end())
        return found->second.matches(source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, error) ? &found->second : nullptr;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The drag selection changed.");
    return nullptr;
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

struct MoveStage
{
    std::string directory;
    ObjectPtr<GFile> item;

    bool create(GFile *entry, const char *pattern, GError **error)
    {
        ObjectPtr<GFile> parent{g_file_get_parent(entry)};
        g_autofree char *path = g_build_filename(g_file_peek_path(parent.get()), pattern, nullptr);
        if (!g_mkdtemp_full(path, 0700))
            return errno_error(errno, error);
        directory = path;
        g_autofree char *name = g_file_get_basename(entry);
        item.reset(g_file_new_build_filename(path, name, nullptr));
        return true;
    }

    ~MoveStage()
    {
        if (!directory.empty())
            rmdir(directory.c_str());
    }
};

static bool copy_claimed_file(GFile *claimed, const DropIdentity &expected, GFile *staged, GFile *destination, GError **error)
{
    if (!expected.matches(claimed, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, error))
        return false;
    const char *path = g_file_peek_path(claimed);
    struct stat before{}, after{};
    if (lstat(path, &before) < 0)
        return errno_error(errno, error);
    const auto flags = GFileCopyFlags(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA);
    if (!g_file_copy(claimed, staged, flags, nullptr, nullptr, nullptr, error))
        return false;
    if (lstat(path, &after) < 0)
        return errno_error(errno, error);
    if (!same_version(before, after))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The source changed while it was being copied.");
        return false;
    }
    return rename_no_replace(staged, destination, error);
}

static bool move_across_devices(GFile *source, GFile *destination, const DropIdentity &expected, GError **error)
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
    MoveStage copy, claim;
    if (!copy.create(destination, ".dicons-XXXXXX", error) || !claim.create(source, ".dicons-move-XXXXXX", error) ||
        !rename_no_replace(source, claim.item.get(), error))
        return false;

    g_autoptr(GError) failure = nullptr;
    if (copy_claimed_file(claim.item.get(), expected, copy.item.get(), destination, &failure))
    {
        if (unlink(g_file_peek_path(claim.item.get())) == 0)
            return true;
        g_set_error(&failure, G_IO_ERROR, G_IO_ERROR_FAILED, "Copied to %s, but could not remove the source: %s", to, g_strerror(errno));
    }
    else
        unlink(g_file_peek_path(copy.item.get()));

    g_autoptr(GError) restore_error = nullptr;
    if (rename_no_replace(claim.item.get(), source, &restore_error))
        g_propagate_error(error, g_steal_pointer(&failure));
    else
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s\nCould not restore %s: %s\nRecover the item from %s.",
                    failure->message, from, restore_error->message, g_file_peek_path(claim.item.get()));
    return false;
}

bool rename_no_replace(GFile *source, GFile *destination, GError **error, const DropIdentity *move_identity)
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
    if (code == EXDEV && move_identity)
        return move_across_devices(source, destination, *move_identity, error);
    return errno_error(code, error);
#else
    return g_file_move(source, destination, GFileCopyFlags(G_FILE_COPY_NO_FALLBACK_FOR_MOVE | G_FILE_COPY_NOFOLLOW_SYMLINKS),
                       nullptr, nullptr, nullptr, error);
#endif
}

ObjectPtr<GFile> rename_into_folder(GFile *source, GFile *folder, const char *name, bool keep_both, GError **error, const DropIdentity *move_identity)
{
    if (!keep_both)
    {
        ObjectPtr<GFile> destination{g_file_get_child(folder, name)};
        if (rename_no_replace(source, destination.get(), error, move_identity))
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
        if (rename_no_replace(source, destination.get(), &attempt, move_identity))
            return destination;
        if (!g_error_matches(attempt, G_IO_ERROR, G_IO_ERROR_EXISTS))
        {
            g_propagate_error(error, g_steal_pointer(&attempt));
            return nullptr;
        }
    }
}

ObjectPtr<GFile> move_into_folder(GFile *source, GFile *folder, bool keep_both, GError **error, const DropIdentity *expected)
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
    const auto identity = expected ? *expected : file_identity(source);
    return rename_into_folder(source, folder, name, keep_both, error, &identity);
}

ObjectPtr<GFile> rename_desktop_item(GFile *source, const char *name, GError **error, const DropIdentity *expected)
{
    const std::string_view filename{name};
    if (filename.empty() || filename == "." || filename == ".." || filename.find('/') != filename.npos)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Enter a filename within the Desktop folder.");
        return nullptr;
    }

    ObjectPtr<GFile> parent{g_file_get_parent(source)};
    ObjectPtr<GFile> destination{g_file_get_child_for_display_name(parent.get(), name, error)};
    if (!destination || (expected && !expected->matches(source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, error)) ||
        !rename_no_replace(source, destination.get(), error))
        return nullptr;
    return destination;
}
