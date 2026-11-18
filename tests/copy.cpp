#include "copy.hpp"

#include <glib/gstdio.h>
#include <sys/stat.h>
#include <filesystem>
#include <utility>

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
    g_test_add_func("/copy/failed-folder", failed_folder);
    g_test_add_func("/copy/recursive-drop", recursive_drop);
    return g_test_run();
}
