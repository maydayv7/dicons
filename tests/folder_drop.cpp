#include "copy.hpp"
#include "file_rename.hpp"
#include "grid_selection.hpp"
#include "trash_file.hpp"

#include <barrier>
#include <csignal>
#include <sys/resource.h>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;

static ObjectPtr<GFile> file(const fs::path &path)
{
    return ObjectPtr<GFile>{g_file_new_for_path(path.c_str())};
}

static std::string contents(const fs::path &path)
{
    std::ifstream input(path);
    g_assert_true(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}

struct Fixture
{
    fs::path desktop = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::path folder = desktop / "Folder";
    Fixture() { fs::create_directories(folder); }

    ObjectPtr<GFile> move(const fs::path &source, const fs::path &target, bool keep_both, GError **error)
    {
        return move_into_folder(file(source).get(), file(target).get(), keep_both, error);
    }
};

static void drag_formats()
{
    Fixture f;
    g_autoptr(GListStore) items = g_list_store_new(G_TYPE_FILE_INFO);
    g_autoptr(GtkMultiSelection) model = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(items)));
    auto *selection = GTK_SELECTION_MODEL(model);
    g_assert_null(selected_file_content(selection));

    std::ofstream(f.desktop / "plain") << "payload";
    fs::create_symlink("plain", f.desktop / "file link");
    fs::create_directory_symlink(f.folder, f.desktop / "folder link");
    fs::create_symlink("missing", f.desktop / "dangling #%");
    std::string expected;
    for (auto name : {"plain", "file link", "folder link", "dangling #%"})
    {
        auto entry = file(f.desktop / name);
        g_autoptr(GFileInfo) info = g_file_query_info(entry.get(), G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
                                                      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        g_assert_nonnull(info);
        g_file_info_set_attribute_object(info, "standard::file", G_OBJECT(entry.get()));
        g_list_store_append(items, info);
        g_autofree char *uri = g_file_get_uri(entry.get());
        expected += std::string(uri) + "\r\n";
    }
    gtk_selection_model_select_item(selection, 0, TRUE);
    g_autoptr(GdkContentProvider) plain = selected_file_content(selection);
    g_autoptr(GdkContentFormats) plain_formats = gdk_content_provider_ref_formats(plain);
    g_assert_true(gdk_content_formats_contain_gtype(plain_formats, GDK_TYPE_FILE_LIST));

    gtk_selection_model_select_all(selection);
    g_autoptr(GdkContentProvider) mixed = selected_file_content(selection);
    g_autoptr(GdkContentFormats) formats = gdk_content_provider_ref_formats(mixed);
    g_assert_true(gdk_content_formats_contain_mime_type(formats, "text/uri-list"));
    g_assert_false(gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST));
    g_autoptr(GOutputStream) output = g_memory_output_stream_new_resizable();
    g_autoptr(GAsyncResult) result = nullptr;
    gdk_content_provider_write_mime_type_async(mixed, "text/uri-list", output, G_PRIORITY_DEFAULT, nullptr, [](GObject *, GAsyncResult *res, gpointer data)
                                               { *static_cast<GAsyncResult **>(data) = G_ASYNC_RESULT(g_object_ref(res)); }, &result);
    while (!result)
        g_main_context_iteration(nullptr, TRUE);
    g_autoptr(GError) error = nullptr;
    g_assert_true(gdk_content_provider_write_mime_type_finish(mixed, result, &error));
    g_assert_no_error(error);
    auto *memory = G_MEMORY_OUTPUT_STREAM(output);
    g_assert_cmpmem(g_memory_output_stream_get_data(memory), g_memory_output_stream_get_data_size(memory),
                    expected.data(), expected.size());
}

static void kinds_and_conflicts()
{
    Fixture f;
    for (const std::string source_kind : {"regular", "directory", "symlink", "dangling"})
    {
        for (const std::string target_kind : {"absent", "regular", "directory", "symlink", "dangling"})
        {
            auto name = source_kind + "-" + target_kind + ".ext";
            auto source = f.desktop / name;
            auto target = f.folder / name;
            auto create = [&](const fs::path &path, const std::string &kind, const char *value)
            {
                if (kind == "regular")
                    std::ofstream(path) << value;
                if (kind == "directory")
                {
                    fs::create_directory(path);
                    std::ofstream(path / "child") << value;
                }
                if (kind == "symlink")
                    fs::create_symlink(f.folder, path);
                if (kind == "dangling")
                    fs::create_symlink("missing", path);
            };
            create(source, source_kind, "source");
            create(target, target_kind, "existing");
            g_autoptr(GError) error = nullptr;
            auto result = f.move(source, f.folder, false, &error);
            if (target_kind != "absent")
            {
                g_assert_null(result);
                g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
                g_clear_error(&error);
                result = f.move(source, f.folder, true, &error);
                if (target_kind == "regular")
                    g_assert_true(contents(target) == "existing");
                if (target_kind == "directory")
                    g_assert_true(contents(target / "child") == "existing");
                if (target_kind == "symlink" || target_kind == "dangling")
                    g_assert_true(fs::is_symlink(target));
                target = f.folder / (source_kind == "directory" ? name + " (1)" : source.stem().string() + " (1).ext");
            }
            g_assert_no_error(error);
            g_assert_nonnull(result);
            g_assert_true(g_file_equal(result.get(), file(target).get()));
            g_assert_true(fs::symlink_status(source).type() == fs::file_type::not_found);
            if (source_kind == "regular")
                g_assert_true(contents(target) == "source");
            if (source_kind == "directory")
                g_assert_true(contents(target / "child") == "source");
            if (source_kind == "symlink")
                g_assert_true(fs::read_symlink(target) == f.folder);
            if (source_kind == "dangling")
                g_assert_true(fs::read_symlink(target) == "missing");
        }
    }
}

static void invalid_targets()
{
    Fixture f;
    auto source = f.desktop / "item";
    std::ofstream(source) << "original";
    fs::create_symlink("missing", f.desktop / "broken");
    fs::create_symlink("loop", f.desktop / "loop");
    std::ofstream(f.desktop / "plain") << "plain";
    for (const auto &name : {"absent", "broken", "loop", "plain"})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_null(f.move(source, f.desktop / name, false, &error));
        g_assert_nonnull(error);
        g_assert_true(contents(source) == "original");
    }
    fs::create_directory(f.folder / "sub");
    fs::create_directory_symlink(f.folder / "sub", f.desktop / "alias");
    for (auto target : {f.folder, f.folder / "sub", f.desktop / "alias"})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_null(f.move(f.folder, target, false, &error));
        g_assert_nonnull(error);
        g_assert_true(fs::is_directory(f.folder / "sub"));
    }

    fs::create_directory_symlink(f.desktop, f.desktop / "back");
    g_autoptr(GError) error = nullptr;
    for (auto target : {f.desktop, f.desktop / "back"})
    {
        g_assert_nonnull(f.move(source, target, true, &error));
        g_assert_no_error(error);
        g_assert_false(fs::exists(f.desktop / "item (1)"));
    }
    fs::remove(source);
    g_assert_null(f.move(source, f.folder, false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void permission_denied()
{
    if (getuid() == 0)
    {
        g_test_skip("Root bypasses directory permissions");
        return;
    }
    Fixture f;
    auto source = f.desktop / "item";
    std::ofstream(source) << "original";
    fs::permissions(f.folder, fs::perms::owner_read | fs::perms::owner_exec);
    g_autoptr(GError) error = nullptr;
    auto result = f.move(source, f.folder, false, &error);
    fs::permissions(f.folder, fs::perms::owner_all);
    g_assert_null(result);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_true(contents(source) == "original");
    g_assert_true(fs::is_empty(f.folder));
}

static DropIdentity identity(const fs::path &path, GFileQueryInfoFlags flags)
{
    ObjectPtr<GFileInfo> info{g_file_query_info(file(path).get(), drop_identity_attributes, flags, nullptr, nullptr)};
    g_assert_nonnull(info);
    return DropIdentity(info.get());
}

static void stale_drag()
{
    Fixture f;
    auto source = f.desktop / "item";
    std::ofstream(source) << "old";
    auto id = identity(source, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS);
    std::ofstream(source) << "edited in place";
    g_autoptr(GError) error = nullptr;
    g_assert_true(id.matches(file(source).get(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, &error));
    g_assert_no_error(error);
    fs::rename(source, f.desktop / "renamed");
    g_assert_false(id.matches(file(source).get(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    std::ofstream(source) << "replacement";
    g_assert_false(id.matches(file(source).get(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    auto alias = f.desktop / "alias";
    fs::create_directory_symlink(f.folder, alias);
    auto link_id = identity(alias, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS);
    auto folder_id = identity(alias, G_FILE_QUERY_INFO_NONE);
    fs::rename(alias, f.desktop / "old-link");
    fs::create_directory_symlink(f.desktop, alias);
    g_assert_false(folder_id.matches(file(alias).get(), G_FILE_QUERY_INFO_NONE, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(link_id.matches(file(alias).get(), G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, &error));
    g_assert_nonnull(error);
}

// Launch both operations from the same barrier. Either winner is valid.
static void race(auto first, auto second)
{
    std::barrier start{2};
    std::jthread worker([&]
                        { start.arrive_and_wait(); first(); });
    start.arrive_and_wait();
    second();
}

static void competing_moves()
{
    Fixture f;
    auto other = f.desktop / "other";
    fs::create_directory(other);
    for (int i = 0; i < 20; ++i)
    {
        auto name = "item" + std::to_string(i);
        auto source = f.desktop / name;
        std::ofstream(source) << "original";
        bool a, b;
        race([&]
             { a = bool(f.move(source, f.folder, false, nullptr)); },
             [&]
             { b = bool(f.move(source, other, false, nullptr)); });
        g_assert_true(a != b);
        g_assert_false(fs::exists(source));
        g_assert_true(contents((a ? f.folder : other) / name) == "original");
    }
}

static void concurrent_collisions()
{
    Fixture f;
    auto other = f.desktop / "other";
    fs::create_directory(other);
    for (bool keep_both : {false, true})
    {
        auto name = keep_both ? "two.txt" : "one.txt";
        std::ofstream(f.desktop / name) << "one";
        std::ofstream(other / name) << "two";
        ObjectPtr<GFile> a, b;
        race([&]
             { a = f.move(f.desktop / name, f.folder, keep_both, nullptr); },
             [&]
             { b = f.move(other / name, f.folder, keep_both, nullptr); });
        if (keep_both)
        {
            g_assert_nonnull(a);
            g_assert_nonnull(b);
            g_assert_false(g_file_equal(a.get(), b.get()));
        }
        else
            g_assert_true(bool(a) != bool(b));
        g_assert_true(contents(a ? fs::path(g_file_peek_path(a.get())) : f.desktop / name) == "one");
        g_assert_true(contents(b ? fs::path(g_file_peek_path(b.get())) : other / name) == "two");
    }
}

static void overlapping_operations()
{
    Fixture f;
    auto stage = f.folder / ".stage";
    fs::create_directory(stage);
    auto other = f.desktop / "other";
    fs::create_directory(other);
    std::ofstream(f.desktop / "same") << "move";
    std::ofstream(other / "same") << "copy";
    bool a, b;
    race([&]
         { a = bool(f.move(f.desktop / "same", f.folder, false, nullptr)); },
         [&]
         { b = bool(desktop_copy_item(file(other / "same").get(), file(f.folder).get(), file(stage).get(), false, nullptr)); });
    g_assert_true(a != b);
    g_assert_true(contents(f.folder / "same") == (a ? "move" : "copy"));
    g_assert_true(fs::exists(f.desktop / "same") == !a);
    g_assert_true(fs::is_empty(stage));

    auto src = f.desktop / "renaming";
    std::ofstream(src) << "rename";
    race([&]
         { a = bool(f.move(src, f.folder, false, nullptr)); },
         [&]
         { b = bool(rename_desktop_item(file(src).get(), "new", nullptr)); });
    g_assert_true(a != b);
    g_assert_true(contents(a ? f.folder / "renaming" : f.desktop / "new") == "rename");

    auto deleting = f.desktop / "deleting";
    std::ofstream(deleting) << "trash";
    TrashedFile entry;
    race([&]
         { a = bool(f.move(deleting, f.folder, false, nullptr)); },
         [&]
         { b = move_to_trash(file(deleting).get(), entry, nullptr); });
    g_assert_true(a != b);
    if (b)
        g_assert_nonnull(restore_from_trash(entry, nullptr));
    g_assert_true(contents(a ? f.folder / "deleting" : deleting) == "trash");
}

static void cross_filesystem()
{
    Fixture f;
    char pattern[] = "/dev/shm/dicons-test-XXXXXX";
    struct stat first{}, second{};
    if (stat(f.desktop.c_str(), &first) || stat("/dev/shm", &second) ||
        first.st_dev == second.st_dev || !g_mkdtemp_full(pattern, 0700))
    {
        g_test_skip("Requires a writable second filesystem");
        return;
    }
    fs::path target{pattern};
    std::ofstream(f.desktop / "item.txt") << "original";
    std::ofstream(f.folder / "child") << "child";
    fs::create_symlink("missing", f.desktop / "link");
    for (bool keep_both : {false, true})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_null(f.move(f.folder, target, keep_both, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    }
    g_assert_true(contents(f.folder / "child") == "child");
    g_assert_true(fs::is_empty(target));
    std::ofstream(target / "item.txt") << "existing";
    g_autoptr(GError) error = nullptr;
    g_assert_null(f.move(f.desktop / "item.txt", target, false, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_nonnull(f.move(f.desktop / "item.txt", target, true, &error));
    g_assert_no_error(error);
    g_assert_true(contents(target / "item.txt") == "existing");
    g_assert_true(contents(target / "item (1).txt") == "original");
    g_assert_false(fs::exists(f.desktop / "item.txt"));
    g_assert_nonnull(f.move(f.desktop / "link", target, false, &error));
    g_assert_no_error(error);
    g_assert_true(fs::read_symlink(target / "link") == "missing");
    g_assert_false(fs::is_symlink(f.desktop / "link"));

    auto source = f.desktop / "large";
    const std::string data(65536, 'x');
    std::ofstream(source) << data;
    auto empty = target / "failures";
    fs::create_directory(empty);
    struct rlimit limit{};
    g_assert_cmpint(getrlimit(RLIMIT_FSIZE, &limit), ==, 0);
    auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
    auto restricted = limit;
    restricted.rlim_cur = std::min<rlim_t>(4096, limit.rlim_cur);
    g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &restricted), ==, 0);
    auto failed = f.move(source, empty, false, &error);
    g_assert_cmpint(setrlimit(RLIMIT_FSIZE, &limit), ==, 0);
    std::signal(SIGXFSZ, old_handler);
    g_assert_null(failed);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(contents(source) == data);
    g_assert_true(fs::is_empty(empty));
    if (getuid() != 0)
    {
        fs::permissions(f.desktop, fs::perms::owner_read | fs::perms::owner_exec);
        failed = f.move(source, empty, false, &error);
        fs::permissions(f.desktop, fs::perms::owner_all);
        g_assert_null(failed);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
        g_clear_error(&error);
        g_assert_true(fs::is_empty(empty));
        g_assert_true(contents(source) == data);
    }

    auto stage = target / ".stage";
    fs::create_directory(stage);
    auto copy = desktop_copy_item(file(f.folder).get(), file(target).get(), file(stage).get(), false, nullptr);
    g_assert_nonnull(copy);
    g_assert_true(contents(target / "Folder/child") == "child");
    g_assert_true(contents(f.folder / "child") == "child");
    fs::remove_all(target);
}

int main(int argc, char **argv)
{
    g_setenv("GIO_USE_VFS", "local", true);
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/folder-drop/drag-formats", drag_formats);
    g_test_add_func("/folder-drop/types-and-conflicts", kinds_and_conflicts);
    g_test_add_func("/folder-drop/invalid-targets", invalid_targets);
    g_test_add_func("/folder-drop/permission-denied", permission_denied);
    g_test_add_func("/folder-drop/stale-drag", stale_drag);
    g_test_add_func("/folder-drop/competing-moves", competing_moves);
    g_test_add_func("/folder-drop/concurrent-collisions", concurrent_collisions);
    g_test_add_func("/folder-drop/overlapping-operations", overlapping_operations);
    g_test_add_func("/folder-drop/cross-filesystem", cross_filesystem);
    return g_test_run();
}
