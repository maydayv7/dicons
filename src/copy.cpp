#include "copy.hpp"
#include "file_rename.hpp"

#include <glib/gstdio.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

static bool list_children(GFile *folder, std::vector<std::string> &names, GError **error)
{
    g_autoptr(GFileEnumerator) children = g_file_enumerate_children(folder, G_FILE_ATTRIBUTE_STANDARD_NAME,
                                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, error);
    if (!children)
        return false;
    for (;;)
    {
        GFileInfo *info;
        if (!g_file_enumerator_iterate(children, &info, nullptr, nullptr, error))
            return false;
        if (!info)
            return g_file_enumerator_close(children, nullptr, error);
        names.emplace_back(g_file_info_get_name(info));
    }
}

static gboolean copy_tree(GFile *source, GFile *target, const char *stage_id, GError **error)
{
    g_autoptr(GFileInfo) info = g_file_query_info(source, "standard::type,id::file,unix::mode,"
                                                          "time::modified,time::modified-usec",
                                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, error);
    if (!info)
        return FALSE;
    GFileType type = g_file_info_get_file_type(info);
    if (type == G_FILE_TYPE_REGULAR || type == G_FILE_TYPE_SYMBOLIC_LINK)
        return g_file_copy(source, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, error);
    if (type != G_FILE_TYPE_DIRECTORY)
    {
        g_autofree char *name = g_file_get_parse_name(source);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Unsupported file type: %s", name);
        return FALSE;
    }

    // Also catch aliases (like bind mounts) for the temporary destination
    if (stage_id && g_strcmp0(stage_id, g_file_info_get_attribute_string(info, "id::file")) == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                            "Cannot copy a folder inside itself.");
        return FALSE;
    }
    if (!g_file_make_directory(target, nullptr, error))
        return FALSE;
    std::vector<std::string> names;
    if (!list_children(source, names, error))
        return FALSE;
    for (const auto &name : names)
    {
        g_autoptr(GFile) child = g_file_get_child(source, name.c_str());
        g_autoptr(GFile) dest = g_file_get_child(target, name.c_str());
        if (!copy_tree(child, dest, stage_id, error))
            return FALSE;
    }
    g_autoptr(GFileInfo) attributes = g_file_info_new();
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE))
        g_file_info_set_attribute_uint32(attributes, G_FILE_ATTRIBUTE_UNIX_MODE,
                                         g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE) & 0777);
    g_autoptr(GDateTime) modified = g_file_info_get_modification_date_time(info);
    if (modified)
        g_file_info_set_modification_date_time(attributes, modified);
    return g_file_set_attributes_from_info(target, attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                           nullptr, error);
}

// Only called on our private staging tree
static gboolean remove_tree(GFile *file, GError **error)
{
    if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) == G_FILE_TYPE_DIRECTORY)
    {
        g_chmod(g_file_peek_path(file), 0700);
        std::vector<std::string> names;
        if (!list_children(file, names, error))
            return FALSE;
        for (const auto &name : names)
        {
            g_autoptr(GFile) child = g_file_get_child(file, name.c_str());
            if (!remove_tree(child, error))
                return FALSE;
        }
    }
    return g_file_delete(file, nullptr, error);
}

ObjectPtr<GFile> desktop_copy_item(GFile *source, GFile *desktop, GFile *stage, bool keep_both,
                                   GError **error)
{
    g_autofree char *name = g_file_get_basename(source);
    const std::filesystem::path filename{name ? name : ""};
    if (filename.empty() || filename.has_parent_path() || filename == "." || filename == "..")
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME, "Invalid source name.");
        return nullptr;
    }
    ObjectPtr<GFile> destination{g_file_get_child(desktop, name)};
    if (!keep_both && g_file_query_file_type(destination.get(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                             nullptr) != G_FILE_TYPE_UNKNOWN)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "The destination already exists.");
        return nullptr;
    }
    const bool directory = g_file_query_file_type(source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                  nullptr) == G_FILE_TYPE_DIRECTORY;
    if (directory && g_file_is_native(source))
    {
        g_autofree char *source_path = realpath(g_file_peek_path(source), nullptr);
        g_autofree char *desktop_path = realpath(g_file_peek_path(desktop), nullptr);
        if (source_path && desktop_path)
        {
            g_autoptr(GFile) from = g_file_new_for_path(source_path);
            g_autoptr(GFile) to = g_file_new_for_path(desktop_path);
            if (g_file_equal(from, to) || g_file_has_prefix(to, from))
            {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                                    "Cannot copy a folder inside itself.");
                return nullptr;
            }
        }
    }

    g_autofree char *unique = g_uuid_string_random();
    g_autoptr(GFile) staged = g_file_get_child(stage, unique);
    g_autoptr(GFileInfo) stage_info = g_file_query_info(stage, "id::file", G_FILE_QUERY_INFO_NONE, nullptr, error);
    if (!stage_info)
        return nullptr;
    gboolean ready = copy_tree(source, staged, g_file_info_get_attribute_string(stage_info, "id::file"), error);
    if (ready)
    {
        if (auto published = rename_into_folder(staged, desktop, name, keep_both, error))
            return published;
    }

    g_autoptr(GError) cleanup = nullptr;
    if (!remove_tree(staged, &cleanup) && !g_error_matches(cleanup, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        g_printerr("Cannot remove temporary copy: %s\n", cleanup->message);
    return nullptr;
}
