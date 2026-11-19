#include "file_rename.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string_view>

bool rename_no_replace(GFile *source, GFile *destination, GError **error)
{
#ifdef __linux__
    if (renameat2(AT_FDCWD, g_file_peek_path(source), AT_FDCWD,
                  g_file_peek_path(destination), RENAME_NOREPLACE) == 0)
        return true;
    int saved_errno = errno;
    g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), g_strerror(saved_errno));
    return false;
#else
    return g_file_move(source, destination, G_FILE_COPY_NO_FALLBACK_FOR_MOVE, nullptr, nullptr, nullptr, error);
#endif
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
