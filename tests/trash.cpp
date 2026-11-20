#include "trash_file.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static TrashedFile trash(const fs::path &path)
{
    ObjectPtr<GFile> file{g_file_new_for_path(path.c_str())};
    TrashedFile entry;
    g_autoptr(GError) error = nullptr;
    g_assert_true(move_to_trash(file.get(), entry, &error));
    g_assert_no_error(error);
    g_assert_nonnull(entry.stored);
    return entry;
}

static fs::path desktop()
{
    fs::create_directories(g_get_home_dir());
    fs::path path = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(path);
    return path;
}

static void restore_items()
{
    const auto folder = desktop();
    const auto path = folder / "file name.txt";
    std::ofstream(path) << "old";
    auto older = trash(path);
    std::ofstream(path) << "restore";
    auto last = trash(path);
    std::ofstream(path) << "keep";
    fs::create_symlink("missing", folder / "file name (1).txt");
    g_autoptr(GError) error = nullptr;
    auto restored = restore_from_trash(last, &error);
    g_assert_no_error(error);
    g_assert_nonnull(restored);
    g_assert_true(fs::path{g_file_peek_path(restored.get())} == folder / "file name (2).txt");
    std::string content;
    std::ifstream(g_file_peek_path(restored.get())) >> content;
    g_assert_cmpstr(content.c_str(), ==, "restore");
    std::ifstream(path) >> content;
    g_assert_cmpstr(content.c_str(), ==, "keep");
    g_assert_true(fs::exists(g_file_peek_path(older.stored.get())));
    g_assert_false(fs::exists(g_file_peek_path(last.metadata.get())));

    const auto dir = folder / "folder.ext";
    fs::create_directories(dir);
    std::ofstream(dir / "inside") << "safe";
    auto directory = trash(dir);
    fs::create_directories(dir);
    g_assert_nonnull(restore_from_trash(directory, &error));
    g_assert_no_error(error);
    g_assert_true(fs::exists(folder / "folder.ext (1)" / "inside"));

    const auto link = folder / ".link";
    fs::create_symlink("absent", link);
    auto dangling = trash(link);
    g_assert_nonnull(restore_from_trash(dangling, &error));
    g_assert_no_error(error);
    g_assert_true(fs::read_symlink(link) == "absent");
}

static void replaced_entry()
{
    const auto path = desktop() / "again";
    std::ofstream(path) << "first";
    auto first = trash(path);
    g_autoptr(GError) error = nullptr;
    g_assert_nonnull(restore_from_trash(first, &error));
    auto second = trash(path); // same file, same Trash basename, different operation
    g_assert_null(restore_from_trash(first, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(fs::exists(path));
    g_assert_nonnull(restore_from_trash(second, &error));
    g_assert_no_error(error);
}

int main(int argc, char **argv)
{
    g_setenv("GIO_USE_VFS", "local", true);
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/trash/restore-items", restore_items);
    g_test_add_func("/trash/replaced-entry", replaced_entry);
    return g_test_run();
}
