#include "file_rename.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void rename_items()
{
    const fs::path desktop = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(desktop / "folder");
    std::ofstream(desktop / "folder" / "inside") << "keep";
    ObjectPtr<GFile> folder{g_file_new_for_path((desktop / "folder").c_str())};
    g_autoptr(GError) error = nullptr;
    auto renamed = rename_desktop_item(folder.get(), "new.folder", &error);
    g_assert_no_error(error);
    g_assert_nonnull(renamed);
    g_assert_true(fs::exists(desktop / "new.folder" / "inside"));
    g_assert_false(fs::exists(desktop / "folder"));

    fs::create_symlink("missing", desktop / "dangling");
    ObjectPtr<GFile> link{g_file_new_for_path((desktop / "dangling").c_str())};
    auto moved = rename_desktop_item(link.get(), "renamed link", &error);
    g_assert_no_error(error);
    g_assert_nonnull(moved);
    g_assert_true(fs::read_symlink(desktop / "renamed link") == "missing");
}

static void preserve_existing()
{
    const fs::path desktop = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(desktop);
    std::ofstream(desktop / "one") << "original";
    std::ofstream(desktop / "two") << "existing";
    fs::create_symlink("missing", desktop / "link");
    ObjectPtr<GFile> source{g_file_new_for_path((desktop / "one").c_str())};
    for (auto name : {"two", "link"})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_null(rename_desktop_item(source.get(), name, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    }
    for (auto name : {"", ".", "..", "../outside", "sub/name", "/outside"})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_null(rename_desktop_item(source.get(), name, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME);
    }
    g_assert_true(fs::is_symlink(desktop / "link"));
    std::string contents;
    std::ifstream(desktop / "one") >> contents;
    g_assert_cmpstr(contents.c_str(), ==, "original");
    std::ifstream(desktop / "two") >> contents;
    g_assert_cmpstr(contents.c_str(), ==, "existing");
}

static void folder_drop()
{
    const fs::path desktop = fs::path(g_get_user_data_dir()) / "Desktop";
    const auto folder = desktop / "Folder";
    fs::create_directories(folder);
    std::ofstream(desktop / "note.txt") << "move me";
    fs::create_hard_link(desktop / "note.txt", desktop / "original");
    ObjectPtr<GFile> target{g_file_new_for_path(folder.c_str())};
    ObjectPtr<GFile> note{g_file_new_for_path((desktop / "note.txt").c_str())};
    g_autoptr(GError) error = nullptr;
    g_assert_nonnull(move_into_folder(note.get(), target.get(), false, &error));
    g_assert_no_error(error);
    g_assert_false(fs::exists(desktop / "note.txt"));
    g_assert_true(fs::equivalent(folder / "note.txt", desktop / "original"));

    std::ofstream(desktop / "note.txt") << "second";
    std::ofstream(folder / "note (1).txt") << "taken";
    g_assert_null(move_into_folder(note.get(), target.get(), false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_true(fs::exists(desktop / "note.txt"));
    g_assert_nonnull(move_into_folder(note.get(), target.get(), true, &error));
    g_assert_no_error(error);
    g_assert_false(fs::exists(desktop / "note.txt"));
    g_assert_true(fs::exists(folder / "note (2).txt"));
    g_assert_true(fs::equivalent(folder / "note.txt", desktop / "original"));

    fs::create_symlink("missing", desktop / "link");
    ObjectPtr<GFile> link{g_file_new_for_path((desktop / "link").c_str())};
    g_assert_nonnull(move_into_folder(link.get(), target.get(), false, &error));
    g_assert_no_error(error);
    g_assert_false(fs::is_symlink(desktop / "link"));
    g_assert_true(fs::read_symlink(folder / "link") == "missing");
    g_assert_null(move_into_folder(target.get(), target.get(), false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/rename/items", rename_items);
    g_test_add_func("/rename/preserve-existing", preserve_existing);
    g_test_add_func("/rename/folder-drop", folder_drop);
    return g_test_run();
}
