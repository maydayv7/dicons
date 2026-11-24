#include "copy.hpp"

#include <glib/gstdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>
#include <unistd.h>

struct Fixture
{
    ObjectPtr<GFile> source{g_file_new_build_filename(g_get_user_data_dir(), "src", "Album", nullptr)};
    ObjectPtr<GFile> desktop{g_file_new_build_filename(g_get_user_data_dir(), "Desktop", nullptr)};
    ObjectPtr<GFile> stage{g_file_get_child(desktop.get(), ".stage")};

    Fixture()
    {
        g_assert_true(g_file_make_directory_with_parents(source.get(), nullptr, nullptr));
        g_assert_true(g_file_make_directory_with_parents(stage.get(), nullptr, nullptr));
    }

    ~Fixture()
    {
        g_assert_true(std::filesystem::is_empty(g_file_peek_path(stage.get())));
    }
};

static void folder_and_collision()
{
    const Fixture fixture;
    g_autoptr(GFile) file = g_file_resolve_relative_path(fixture.source.get(), "nested/song.txt");
    g_autoptr(GFile) nested = g_file_get_parent(file);
    g_assert_true(g_file_make_directory(nested, nullptr, nullptr));
    g_assert_true(g_file_set_contents(g_file_peek_path(file), "original", -1, nullptr));
    g_autoptr(GError) error = nullptr;
    auto original = desktop_copy_item(fixture.source.get(), fixture.desktop.get(), fixture.stage.get(), false, &error);
    g_assert_no_error(error);
    g_assert_nonnull(original);

    g_assert_true(g_file_set_contents(g_file_peek_path(file), "updated", -1, nullptr));
    g_assert_null(desktop_copy_item(fixture.source.get(), fixture.desktop.get(), fixture.stage.get(), false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    auto second = desktop_copy_item(fixture.source.get(), fixture.desktop.get(), fixture.stage.get(), true, &error);
    g_assert_no_error(error);
    g_autoptr(GFile) expected = g_file_get_child(fixture.desktop.get(), "Album (1)");
    g_assert_true(g_file_equal(second.get(), expected));

    g_autoptr(GFile) first_song = g_file_resolve_relative_path(original.get(), "nested/song.txt");
    g_autoptr(GFile) second_song = g_file_resolve_relative_path(second.get(), "nested/song.txt");
    g_autofree char *first_text = nullptr;
    g_autofree char *second_text = nullptr;
    g_assert_true(g_file_load_contents(first_song, nullptr, &first_text, nullptr, nullptr, nullptr));
    g_assert_true(g_file_load_contents(second_song, nullptr, &second_text, nullptr, nullptr, nullptr));
    g_assert_cmpstr(first_text, ==, "original");
    g_assert_cmpstr(second_text, ==, "updated");
}

static void collision_filenames()
{
    const Fixture fixture;
    constexpr std::pair<const char *, const char *> cases[]{
        {".profile", ".profile (1)"},
        {"archive.tar.gz", "archive.tar (1).gz"},
        {"README", "README (1)"},
    };
    for (const auto &[name, expected] : cases)
    {
        ObjectPtr<GFile> file{g_file_get_child(fixture.source.get(), name)};
        g_assert_true(g_file_set_contents(g_file_peek_path(file.get()), "data", -1, nullptr));
        g_autoptr(GError) error = nullptr;
        auto first = desktop_copy_item(file.get(), fixture.desktop.get(), fixture.stage.get(), false, &error);
        g_assert_no_error(error);
        g_assert_nonnull(first);
        auto second = desktop_copy_item(file.get(), fixture.desktop.get(), fixture.stage.get(), true, &error);
        g_assert_no_error(error);
        g_assert_nonnull(second);
        g_autofree char *basename = g_file_get_basename(second.get());
        g_assert_cmpstr(basename, ==, expected);
    }
}

static void failed_folder()
{
    const Fixture fixture;
    g_autoptr(GFile) pipe = g_file_get_child(fixture.source.get(), "pipe");
    g_assert_cmpint(mkfifo(g_file_peek_path(pipe), 0600), ==, 0);
    g_autoptr(GError) error = nullptr;
    g_assert_null(desktop_copy_item(fixture.source.get(), fixture.desktop.get(), fixture.stage.get(), false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_autoptr(GFile) target = g_file_get_child(fixture.desktop.get(), "Album");
    g_assert_false(g_file_query_exists(target, nullptr));
    g_assert_true(g_file_delete(pipe, nullptr, nullptr));
}

static ObjectPtr<GFile> copy_with_fd_limit(const Fixture &fixture, bool keep_both, GError **error)
{
    struct rlimit original{};
    g_assert_cmpint(getrlimit(RLIMIT_NOFILE, &original), ==, 0);
    auto limited = original;
    limited.rlim_cur = std::min<rlim_t>(48, original.rlim_cur);
    g_assert_cmpint(setrlimit(RLIMIT_NOFILE, &limited), ==, 0);
    auto result = desktop_copy_item(fixture.source.get(), fixture.desktop.get(), fixture.stage.get(), keep_both, error);
    g_assert_cmpint(setrlimit(RLIMIT_NOFILE, &original), ==, 0);
    return result;
}

static void deep_tree()
{
    namespace fs = std::filesystem;
    const Fixture fixture;
    fs::path branch;
    for (int level = 0; level < 72; ++level)
        branch /= "dir";
    const fs::path source = g_file_peek_path(fixture.source.get());
    const fs::path desktop = g_file_peek_path(fixture.desktop.get());
    const auto leaf = source / branch;
    fs::create_directories(leaf);
    std::ofstream(leaf / ".payload") << "original";
    fs::create_symlink(".payload", leaf / "relative");
    fs::create_symlink("missing", leaf / "dangling");
    fs::create_directory_symlink(source, leaf / "ancestor");
    fs::permissions(leaf, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
    struct stat original{};
    g_assert_cmpint(stat(leaf.c_str(), &original), ==, 0);

    g_autoptr(GError) error = nullptr;
    auto copy = copy_with_fd_limit(fixture, false, &error);
    g_assert_no_error(error);
    g_assert_nonnull(copy);
    const auto copied_leaf = desktop / "Album" / branch;
    struct stat copied{};
    g_assert_cmpint(stat(copied_leaf.c_str(), &copied), ==, 0);
    g_assert_cmpuint(copied.st_mode & 0777, ==, original.st_mode & 0777);
    g_assert_cmpint(copied.st_mtime, ==, original.st_mtime);
    std::string contents;
    std::ifstream(copied_leaf / ".payload") >> contents;
    g_assert_true(contents == "original");
    g_assert_true(fs::read_symlink(copied_leaf / "relative") == ".payload");
    g_assert_true(fs::read_symlink(copied_leaf / "dangling") == "missing");
    g_assert_true(fs::read_symlink(copied_leaf / "ancestor") == source);
    g_assert_true(fs::is_empty(g_file_peek_path(fixture.stage.get())));

    g_assert_cmpint(mkfifo((leaf / "pipe").c_str(), 0600), ==, 0);
    g_assert_null(copy_with_fd_limit(fixture, true, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_true(fs::is_empty(g_file_peek_path(fixture.stage.get())));
    g_assert_false(fs::exists(desktop / "Album (1)"));
    g_assert_true(fs::is_fifo(leaf / "pipe"));
    g_assert_true(fs::read_symlink(leaf / "ancestor") == source);
    g_assert_true(fs::exists(leaf / ".payload"));
    g_assert_true(fs::exists(copied_leaf / ".payload"));
}

static void full_length_names()
{
    const Fixture f;
    const long limit = pathconf(g_file_peek_path(f.desktop.get()), _PC_NAME_MAX);
    g_assert_cmpint(limit, >, 10);
    std::string multibyte;
    while (multibyte.size() + 3 <= size_t(limit) - 4)
        multibyte += "雪";
    for (const auto &name : {multibyte + ".txt", "a." + std::string(limit - 2, 'x')})
    {
        ObjectPtr<GFile> source{g_file_get_child(f.source.get(), name.c_str())};
        g_assert_true((std::ofstream(g_file_peek_path(source.get())) << "test").good());
        auto first = desktop_copy_item(source.get(), f.desktop.get(), f.stage.get(), false, nullptr);
        g_assert_nonnull(first);
        for (guint n : {1, 2})
        {
            auto numbered = desktop_copy_item(source.get(), f.desktop.get(), f.stage.get(), true, nullptr);
            g_assert_nonnull(numbered);
            g_autofree char *actual = g_file_get_basename(numbered.get());
            g_assert_true(g_utf8_validate(actual, -1, nullptr));
            g_assert_true(std::string{actual}.ends_with(" (" + std::to_string(n) + ")" + (name.ends_with(".txt") ? ".txt" : "")));
        }
    }
}

static void recursive_drop()
{
    const Fixture fixture;
    g_autoptr(GFile) ancestor = g_file_get_parent(fixture.desktop.get());
    g_autoptr(GError) error = nullptr;
    g_assert_null(desktop_copy_item(ancestor, fixture.desktop.get(), fixture.stage.get(), false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/copy/folder-and-collision", folder_and_collision);
    g_test_add_func("/copy/collision-filenames", collision_filenames);
    g_test_add_func("/copy/full-length-names", full_length_names);
    g_test_add_func("/copy/failed-folder", failed_folder);
    g_test_add_func("/copy/deep-tree", deep_tree);
    g_test_add_func("/copy/recursive-drop", recursive_drop);
    return g_test_run();
}
