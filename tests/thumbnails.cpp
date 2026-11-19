#include "thumbnail_cache.hpp"

#include <filesystem>
#include <fstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

static void write_image(const fs::path &path, guint32 color)
{
    ObjectPtr<GdkPixbuf> pixels{gdk_pixbuf_new(GDK_COLORSPACE_RGB, false, 8, 40, 20)};
    gdk_pixbuf_fill(pixels.get(), color);
    g_assert_true(gdk_pixbuf_save(pixels.get(), path.c_str(), "png", nullptr, nullptr));
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
        ObjectPtr<GFileInfo> info{g_file_query_info(source, "standard::size,standard::content-type,time::*",
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

    ObjectPtr<GFile> image{g_file_new_for_path(fixture.c_str())};
    auto image_version = version_for(image.get());
    image_version.mime = "image/png";
    auto direct = load_thumbnail(image.get(), image_version, cancel.get());
    g_assert_nonnull(direct.get());
    g_assert_cmpint(gdk_pixbuf_get_width(direct.get()), ==, 2 * gdk_pixbuf_get_height(direct.get()));
    g_cancellable_cancel(cancel.get());
    g_assert_null(load_thumbnail(image.get(), image_version, cancel.get()).get());
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/thumbnails/generate-cache-invalidate", previews);
    return g_test_run();
}
