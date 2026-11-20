#include "trash_file.hpp"
#include "file_rename.hpp"

#include <filesystem>
#include <format>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

static bool same_file(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static bool same_record(GFile *metadata, const struct stat &expected)
{
    struct stat current{};
    return lstat(g_file_peek_path(metadata), &current) == 0 && same_file(current, expected) &&
           current.st_ctim.tv_sec == expected.st_ctim.tv_sec && current.st_ctim.tv_nsec == expected.st_ctim.tv_nsec;
}

bool move_to_trash(GFile *file, TrashedFile &entry, GError **error)
{
    entry.original = retain(file);
    // GIO chooses the Trash and resolves name collisions
    // Follow the original directory entry across its rename
    const char *path = g_file_peek_path(file);
#ifdef __linux__
    int fd = path ? open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1;
#else
    int fd = -1;
#endif
    bool moved = g_file_trash(file, nullptr, error);
    if (fd < 0)
        return moved;
    g_autofree char *stored = nullptr;
    if (moved && fstat(fd, &entry.identity) == 0)
        stored = g_file_read_link(std::format("/proc/self/fd/{}", fd).c_str(), nullptr);
    close(fd);
    if (stored)
    {
        fs::path location{stored};
        if (location.parent_path().filename() == "files" && location != path)
        {
            auto metadata = location.parent_path().parent_path() / "info" / (location.filename().native() + ".trashinfo");
            struct stat current{};
            if (lstat(stored, &current) == 0 && same_file(current, entry.identity) &&
                lstat(metadata.c_str(), &entry.record) == 0 && S_ISREG(entry.record.st_mode))
            {
                entry.stored.reset(g_file_new_for_path(stored));
                entry.metadata.reset(g_file_new_for_path(metadata.c_str()));
            }
        }
    }
    return moved;
}

ObjectPtr<GFile> restore_from_trash(const TrashedFile &entry, GError **error)
{
    struct stat current{};
    if (!entry.stored || lstat(g_file_peek_path(entry.stored.get()), &current) != 0 ||
        !same_file(current, entry.identity) || !same_record(entry.metadata.get(), entry.record))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "The original Trash entry is no longer available.");
        return {};
    }

    ObjectPtr<GFile> parent{g_file_get_parent(entry.original.get())};
    fs::path name = fs::path(g_file_peek_path(entry.original.get())).filename();
    auto stem = S_ISDIR(current.st_mode) ? name : name.stem();
    auto extension = S_ISDIR(current.st_mode) ? fs::path{} : name.extension();
    auto destination = retain(entry.original.get());
    for (guint n = 1; !rename_no_replace(entry.stored.get(), destination.get(), error); ++n)
    {
        if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            return {};
        g_clear_error(error);
        auto numbered = std::format("{} ({}){}", stem.native(), n, extension.native());
        destination.reset(g_file_get_child(parent.get(), numbered.c_str()));
    }
    // Already restored even if cleanup fails
    if (same_record(entry.metadata.get(), entry.record))
        g_file_delete(entry.metadata.get(), nullptr, error);
    return destination;
}
