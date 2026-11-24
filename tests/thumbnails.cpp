#include "thumbnail_cache.hpp"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

static void write_image(const fs::path &path, guint32 color)
{
    ObjectPtr<GdkPixbuf> pixels{gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 40, 20)};
    gdk_pixbuf_fill(pixels.get(), color);
    g_autoptr(GError) error = nullptr;
    const bool saved = gdk_pixbuf_save(pixels.get(), path.c_str(), "png", &error, nullptr);
    g_assert_no_error(error);
    g_assert_true(saved);
}

static void previews()
{
    const fs::path data = g_get_user_data_dir();
    fs::create_directories(data / "thumbnailers");
    const auto path = data / "page % '1.pdf";
    std::ofstream(path) << "document for the test helper";
    const auto fixture = data / "fixture.png";
    write_image(fixture, 0xaa1122ff);

    const auto helper = data / "fixture tools" / "cp";
    fs::create_directory(helper.parent_path());
    g_autofree char *cp = g_find_program_in_path("cp");
    fs::create_symlink(cp, helper);
    fs::copy_file(fixture, path.string() + ".png");
    g_autofree char *quoted = g_shell_quote(helper.c_str());
    std::ofstream(data / "thumbnailers/test.thumbnailer") << "[Thumbnailer Entry]\nExec=" << quoted << " %i.png %o\nMimeType=application/pdf;\n";

    ObjectPtr<GFile> file{g_file_new_for_path(path.c_str())};
    ObjectPtr<GCancellable> cancel{g_cancellable_new()};
    auto version_for = [](GFile *source)
    {
        ObjectPtr<GFileInfo> info{g_file_query_info(source, "standard::size,standard::content-type,time::*,id::file",
                                                    G_FILE_QUERY_INFO_NONE, nullptr, nullptr)};
        return ThumbnailVersion{info.get()};
    };
    auto version = version_for(file.get());
    version.mime = "application/pdf";
    auto pixels = load_thumbnail(file.get(), version, cancel.get());
    g_assert_nonnull(pixels.get());
    g_assert_cmpuint(gdk_pixbuf_read_pixels(pixels.get())[0], ==, 0xaa);
    const auto cache = fs::path(g_get_user_cache_dir()) / "thumbnails/normal";
    const auto entry = fs::directory_iterator(cache)->path();
    struct stat st{};
    g_assert_cmpint(stat(entry.c_str(), &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0077, ==, 0);

    fs::remove(helper);
    g_assert_nonnull(load_thumbnail(file.get(), version, cancel.get()).get());
    auto later = version;
    ++later.nanoseconds;
    g_assert_null(load_thumbnail(file.get(), later, cancel.get()).get());
    std::ofstream(entry) << "incomplete PNG";
    g_assert_null(load_thumbnail(file.get(), version, cancel.get()).get());

    auto replacement = data / "new.png";
    write_image(replacement, 0x2211aaff);
    auto size = std::max(fs::file_size(fixture), fs::file_size(replacement));
    fs::resize_file(fixture, size);
    fs::resize_file(replacement, size);
    ObjectPtr<GFile> image{g_file_new_for_path(fixture.c_str())};
    auto image_version = version_for(image.get());
    image_version.mime = "image/png";
    auto direct = load_thumbnail(image.get(), image_version, cancel.get());
    g_assert_nonnull(direct.get());
    g_assert_cmpint(gdk_pixbuf_get_width(direct.get()), ==, 2 * gdk_pixbuf_get_height(direct.get()));
    fs::last_write_time(replacement, fs::last_write_time(fixture));
    fs::rename(replacement, fixture);
    auto replaced = version_for(image.get());
    replaced.mime = "image/png";
    g_assert_cmpuint(replaced.nanoseconds, ==, image_version.nanoseconds);
    g_assert_true(replaced != image_version);
    auto refreshed = load_thumbnail(image.get(), replaced, cancel.get(), true);
    g_assert_nonnull(refreshed.get());
    g_assert_cmpuint(gdk_pixbuf_read_pixels(refreshed.get())[0], ==, 0x22);
    g_cancellable_cancel(cancel.get());
    g_assert_null(load_thumbnail(image.get(), image_version, cancel.get()).get());
}

int main(int argc, char **argv)
{
    // Keep system MIME data and image codecs visible while isolating files
    g_autofree char *root = g_dir_make_tmp("dicons-thumbnails-XXXXXX", nullptr);
    g_assert_nonnull(root);
    const fs::path dir = root;
    g_setenv("XDG_DATA_HOME", (dir / "data").c_str(), true);
    g_setenv("XDG_CACHE_HOME", (dir / "cache").c_str(), true);
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/thumbnails/generate-cache-invalidate", previews);
    int status = g_test_run();
    fs::remove_all(dir);
    return status;
}
