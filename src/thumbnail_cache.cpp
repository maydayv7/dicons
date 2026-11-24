#include "thumbnail_cache.hpp"
#include "desktop_item.hpp"
#include "symlink.hpp"

#include <glib/gstdio.h>

namespace
{
    constexpr int thumbnail_size = 128;
    constexpr auto timestamp_attributes = "standard::size,time::modified,time::modified-nsec,id::file";
    constexpr auto ns_key = "tEXt::X-dicons::MTimeNS";
    constexpr auto id_key = "tEXt::X-dicons::FileID";

    const char *source_file_id(GFileInfo *info)
    {
        const char *id = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_ID_FILE);
        return id ? id : "";
    }

    bool current_version(GFile *file, const ThumbnailVersion &version, GCancellable *cancel)
    {
        ObjectPtr<GFileInfo> info{g_file_query_info(file, timestamp_attributes, G_FILE_QUERY_INFO_NONE, cancel, nullptr)};
        return info && version.size == g_file_info_get_size(info.get()) &&
               version.file_id == source_file_id(info.get()) &&
               version.modified == g_file_info_get_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
               version.nanoseconds == g_file_info_get_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC);
    }

    ObjectPtr<GdkPixbuf> cached(const char *root, const char *filename, const char *uri,
                                const ThumbnailVersion &version, bool changed)
    {
        for (const char *size : {"normal", "large", "x-large", "xx-large"})
        {
            g_autofree char *path = g_build_filename(root, size, filename, nullptr);
            ObjectPtr<GdkPixbuf> image{gdk_pixbuf_new_from_file_at_scale(path, thumbnail_size, thumbnail_size, true, nullptr)};
            if (!image)
                continue;
            auto option = [&](const char *name)
            { return gdk_pixbuf_get_option(image.get(), name); };
            const char *ns = option(ns_key);
            const char *length = option("tEXt::Thumb::Size");
            const char *id = option(id_key);
            if (g_strcmp0(option("tEXt::Thumb::URI"), uri) == 0 &&
                g_strcmp0(option("tEXt::Thumb::MTime"), std::to_string(version.modified).c_str()) == 0 &&
                (!length || std::to_string(version.size) == length) &&
                (id ? version.file_id == id : !changed) &&
                (ns ? std::to_string(version.nanoseconds) == ns : !changed))
                return image;
        }
        return {};
    }

    void save(const char *root, const char *filename, const char *uri, GdkPixbuf *image,
              const ThumbnailVersion &version, GCancellable *cancel)
    {
        g_autofree char *dir = g_build_filename(root, "normal", nullptr);
        if (g_mkdir_with_parents(dir, 0700) == -1)
            return;
        g_autofree char *png = nullptr;
        gsize length = 0;
        if (!gdk_pixbuf_save_to_buffer(image, &png, &length, "png", nullptr,
                                       "tEXt::Thumb::URI", uri, "tEXt::Thumb::MTime", std::to_string(version.modified).c_str(),
                                       "tEXt::Thumb::Size", std::to_string(version.size).c_str(),
                                       ns_key, std::to_string(version.nanoseconds).c_str(), id_key, version.file_id.c_str(), nullptr))
            return;
        g_autoptr(GFile) output = g_file_new_build_filename(dir, filename, nullptr);
        g_file_replace_contents(output, png, length, nullptr, false,
                                GFileCreateFlags(G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION),
                                nullptr, cancel, nullptr);
    }
}

ThumbnailVersion::ThumbnailVersion(GFileInfo *info)
    : mime(g_file_info_get_content_type(info) ? g_file_info_get_content_type(info) : ""),
      target_path(symlink_target_path(info) ? symlink_target_path(info) : ""),
      modified(g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)),
      nanoseconds(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC)),
      size(g_file_info_get_size(info)), file_id(source_file_id(info))
{
}

bool can_preview(GFileInfo *info)
{
    if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || is_desktop_launcher(info))
        return false;
    if (g_file_info_get_is_symlink(info) && !symlink_target_path(info))
        return false;
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) &&
        !g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
        return false;
    const char *type = g_file_info_get_content_type(info);
    return type && !g_str_equal(type, "application/x-desktop") &&
           (g_str_has_prefix(type, "image/") || g_str_has_prefix(type, "text/") || g_str_has_prefix(type, "application/"));
}

ObjectPtr<GdkPixbuf> load_thumbnail(GFile *file, const ThumbnailVersion &version, GCancellable *cancel, bool changed)
{
    if (!g_file_is_native(file) || g_cancellable_is_cancelled(cancel))
        return {};
    // Check read access before using another app's cached preview
    g_autoptr(GFileInputStream) stream = g_file_read(file, cancel, nullptr);
    if (!stream)
        return {};
    g_autofree char *uri = g_file_get_uri(file);
    g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    g_autofree char *name = g_strconcat(hash, ".png", nullptr);
    g_autofree char *root = g_build_filename(g_get_user_cache_dir(), "thumbnails", nullptr);
    g_autoptr(GFile) cache_dir = g_file_new_for_path(root);
    if (g_file_has_prefix(file, cache_dir))
        return {};
    if (auto image = cached(root, name, uri, version, changed))
        return image;

    ObjectPtr<GdkPixbuf> image;
    if (version.mime.starts_with("image/"))
    {
        ObjectPtr<GdkPixbuf> decoded{gdk_pixbuf_new_from_stream_at_scale(G_INPUT_STREAM(stream), thumbnail_size,
                                                                         thumbnail_size, true, cancel, nullptr)};
        if (decoded)
            image.reset(gdk_pixbuf_apply_embedded_orientation(decoded.get()));
    }
    if (!image)
        image = run_thumbnailer(file, version.mime.c_str(), thumbnail_size, cancel);
    if (!image || !current_version(file, version, cancel))
        return {};
    save(root, name, uri, image.get(), version, cancel);
    return image;
}
