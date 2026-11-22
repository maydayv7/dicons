#include "undo.hpp"
#include "desktop_sort.hpp"
#include "grid_layout.hpp"
#include "preferences.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace
{
    struct Fixture
    {
        ObjectPtr<GApplication> app{g_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
        fs::path root = fs::path(g_get_user_data_dir()) / "Desktop";

        Fixture()
        {
            fs::create_directories(g_get_home_dir());
            fs::create_directories(root);
            g_assert_true(g_application_register(app.get(), nullptr, nullptr));
        }
        ObjectPtr<GFile> file(const char *name) { return ObjectPtr<GFile>{g_file_new_for_path((root / name).c_str())}; }
    };

    void wait_for(auto condition)
    {
        auto deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
        while (!condition() && g_get_monotonic_time() < deadline)
        {
            g_main_context_iteration(nullptr, false);
            g_usleep(1000);
        }
        g_assert_true(condition());
        while (g_main_context_iteration(nullptr, false))
        {
        }
    }
}

static void mixed_history()
{
    Fixture f;
    auto old = f.file("old"), named = f.file("named"), moved = f.file("Folder/named (1)");
    fs::create_directory(f.root / "Folder");
    std::ofstream(f.root / "old") << "keep";
    auto created = begin_undo_step(f.app.get());
    remember_file(created, old.get());
    finish_undo_step(f.app.get(), created);
    for (auto [from, to] : {std::pair{old.get(), named.get()}, std::pair{named.get(), moved.get()}})
    {
        auto step = begin_undo_step(f.app.get());
        g_assert_true(rename_no_replace(from, to, nullptr));
        remember_file(step, to, from);
        finish_undo_step(f.app.get(), step);
    }
    auto deleted = begin_undo_step(f.app.get());
    TrashedFile trash;
    g_assert_true(move_to_trash(moved.get(), trash, nullptr));
    remember_trash(deleted, std::move(trash));
    // Ctrl+Z can arrive before the last worker publishes its result
    undo_latest(f.app.get());
    finish_undo_step(f.app.get(), deleted);
    wait_for([&]
             { return fs::exists(f.root / "Folder/named (1)"); });
    for (auto target : {"named", "old"})
    {
        undo_latest(f.app.get());
        wait_for([&]
                 { return fs::exists(f.root / target); });
    }
    undo_latest(f.app.get());
    wait_for([&]
             { return !fs::exists(f.root / "old"); });
}

static void partial_batch()
{
    Fixture f;
    auto group = begin_undo_step(f.app.get());
    for (auto name : {"one", "two", "three"})
    {
        auto file = f.file(name);
        std::ofstream(f.root / name) << name;
        remember_file(group, file.get());
    }
    finish_undo_step(f.app.get(), group);
    fs::rename(f.root / "two", f.root / "original two");
    std::ofstream(f.root / "two") << "replacement";
    auto skipped = begin_undo_step(f.app.get());
    finish_undo_step(f.app.get(), skipped);
    undo_latest(f.app.get());
    wait_for([&]
             { return !fs::exists(f.root / "one") && !fs::exists(f.root / "three"); });
    g_assert_true(fs::exists(f.root / "two"));
    g_assert_true(fs::exists(f.root / "original two"));
}

static void collision_retry()
{
    Fixture f;
    std::ofstream(f.root / "source") << "original";
    auto source = f.file("source"), destination = f.file("destination");
    auto step = begin_undo_step(f.app.get());
    g_assert_true(rename_no_replace(source.get(), destination.get(), nullptr));
    remember_file(step, destination.get(), source.get());
    finish_undo_step(f.app.get(), step);
    std::ofstream(f.root / "source") << "collision";
    undo_latest(f.app.get());
    // Queue an observable view undo behind the attempted file undo
    auto settings = desktop_preferences();
    change_view(f.app.get(), settings.get(), nullptr, [&]
                { g_settings_set_int(settings.get(), "icon-size", 64); });
    undo_latest(f.app.get());
    wait_for([&]
             { return g_settings_get_int(settings.get(), "icon-size") == 48; });
    g_assert_true(fs::exists(f.root / "destination"));
    fs::remove(f.root / "source");
    undo_latest(f.app.get());
    wait_for([&]
             { return !fs::exists(f.root / "destination"); });
    std::string actual;
    std::ifstream(f.root / "source") >> actual;
    g_assert_true(actual == "original");
}

static void numbered_restore()
{
    Fixture f;
    auto file = f.file("note.txt");
    std::ofstream(f.root / "note.txt") << "created";
    auto create = begin_undo_step(f.app.get());
    remember_file(create, file.get());
    TrashedFile trash;
    auto deletion = begin_undo_step(f.app.get());
    g_assert_true(move_to_trash(file.get(), trash, nullptr));
    remember_trash(deletion, std::move(trash));
    finish_undo_step(f.app.get(), deletion);
    std::ofstream(f.root / "note.txt") << "replacement";
    undo_latest(f.app.get());
    wait_for([&]
             { return fs::exists(f.root / "note (1).txt"); });
    undo_latest(f.app.get());
    finish_undo_step(f.app.get(), create);
    wait_for([&]
             { return !fs::exists(f.root / "note (1).txt"); });
    std::string actual;
    std::ifstream(f.root / "note.txt") >> actual;
    g_assert_true(actual == "replacement");
}

static void original_folder_unavailable()
{
    Fixture f;
    auto settings = desktop_preferences();
    for (bool trash : {false, true})
    {
        fs::create_directory(f.root / "Folder");
        auto original = f.file("Folder/note");
        auto moved = f.file("moved");
        std::ofstream(f.root / "Folder/note") << "original";
        auto step = begin_undo_step(f.app.get());
        if (trash)
        {
            TrashedFile entry;
            g_assert_true(move_to_trash(original.get(), entry, nullptr));
            remember_trash(step, std::move(entry));
        }
        else
        {
            g_assert_true(rename_no_replace(original.get(), moved.get(), nullptr));
            remember_file(step, moved.get(), original.get());
        }
        finish_undo_step(f.app.get(), step);
        fs::rename(f.root / "Folder", f.root / "away");
        for (bool replacement : {false, true})
        {
            if (replacement)
                fs::create_directory(f.root / "Folder");
            undo_latest(f.app.get());
            change_view(f.app.get(), settings.get(), nullptr, [&]
                        { g_settings_set_int(settings.get(), "icon-size", 64); });
            undo_latest(f.app.get());
            wait_for([&]
                     { return g_settings_get_int(settings.get(), "icon-size") == 48; });
            g_assert_false(fs::exists(f.root / "Folder/note"));
        }
        fs::remove(f.root / "Folder");
        fs::rename(f.root / "away", f.root / "Folder");
        undo_latest(f.app.get());
        wait_for([&]
                 { return fs::exists(f.root / "Folder/note"); });
        fs::remove_all(f.root / "Folder");
    }
}

static void numbered_folder_restore()
{
    Fixture f;
    fs::create_directory(f.root / "Folder");
    std::ofstream(f.root / "Folder/child") << "copy";
    auto copy = begin_undo_step(f.app.get());
    remember_file(copy, f.file("Folder/child").get());
    finish_undo_step(f.app.get(), copy);
    auto deletion = begin_undo_step(f.app.get());
    TrashedFile trash;
    g_assert_true(move_to_trash(f.file("Folder").get(), trash, nullptr));
    remember_trash(deletion, std::move(trash));
    finish_undo_step(f.app.get(), deletion);
    fs::create_directory(f.root / "Folder");
    std::ofstream(f.root / "Folder/child") << "unrelated";
    undo_latest(f.app.get());
    wait_for([&]
             { return fs::exists(f.root / "Folder (1)/child"); });
    undo_latest(f.app.get());
    wait_for([&]
             { return fs::is_empty(f.root / "Folder (1)"); });
    g_assert_true(fs::exists(f.root / "Folder/child"));
}

static void history_limit()
{
    Fixture f;
    auto settings = desktop_preferences();
    auto pending = begin_undo_step(f.app.get());
    auto file = f.file("older-file");
    std::ofstream(f.root / "older-file") << "keep";
    remember_file(pending, file.get());
    g_settings_set_int(settings.get(), "icon-size", 48);
    for (int size = 49; size <= 84; ++size)
        change_view(f.app.get(), settings.get(), nullptr, [&]
                    { g_settings_set_int(settings.get(), "icon-size", size); });

    finish_undo_step(f.app.get(), pending);
    auto failed = begin_undo_step(f.app.get());
    finish_undo_step(f.app.get(), failed);
    for (int size = 83; size >= 52; --size)
    {
        undo_latest(f.app.get());
        g_assert_cmpint(g_settings_get_int(settings.get(), "icon-size"), ==, size);
    }
    undo_latest(f.app.get());
    g_assert_cmpint(g_settings_get_int(settings.get(), "icon-size"), ==, 52);
    g_assert_true(fs::exists(f.root / "older-file"));
}

static void retry_at_history_limit()
{
    Fixture f;
    auto original = f.file("original"), moved = f.file("moved");
    std::ofstream(f.root / "original") << "item";
    auto step = begin_undo_step(f.app.get());
    g_assert_true(rename_no_replace(original.get(), moved.get(), nullptr));
    remember_file(step, moved.get(), original.get());
    finish_undo_step(f.app.get(), step);
    std::weak_ptr<UndoStep> retry = step;
    step.reset();
    std::ofstream(f.root / "original") << "replacement";
    undo_latest(f.app.get());

    auto settings = desktop_preferences();
    g_settings_set_int(settings.get(), "icon-size", 48);
    for (int size = 49; size <= 80; ++size)
        change_view(f.app.get(), settings.get(), nullptr, [&]
                    { g_settings_set_int(settings.get(), "icon-size", size); });
    wait_for([&]
             { return retry.expired(); });
    for (int i = 0; i < 32; ++i)
        undo_latest(f.app.get());
    g_assert_cmpint(g_settings_get_int(settings.get(), "icon-size"), ==, 48);
    g_assert_true(fs::exists(f.root / "moved"));
}

static void views()
{
    Fixture f;
    auto settings = desktop_preferences();
    DesktopSort sorting(settings.get());
    ObjectPtr<GSimpleAction> hidden{g_simple_action_new_stateful("show-hidden", nullptr, g_variant_new_boolean(false))};
    auto toggle = undoable_view_action(G_ACTION(hidden.get()), f.app.get(), settings.get(), G_ACTION(hidden.get()));
    g_action_activate(toggle.get(), nullptr);
    g_autoptr(GVariant) enabled = g_action_get_state(G_ACTION(hidden.get()));
    g_assert_true(g_variant_get_boolean(enabled));
    auto sort = undoable_view_action(g_action_map_lookup_action(G_ACTION_MAP(sorting.actions.get()), "field"), f.app.get(), settings.get(), G_ACTION(hidden.get()));
    change_view(f.app.get(), settings.get(), nullptr, [&]
                { write_positions(settings.get(), {{"file:///item", {4, 2}}}); });
    g_action_activate(sort.get(), g_variant_new_string("modified"));
    g_assert_true(read_positions(settings.get()).empty());
    undo_latest(f.app.get());
    g_autoptr(GVariant) field = g_action_get_state(sort.get());
    g_assert_cmpstr(g_variant_get_string(field, nullptr), ==, "name");
    g_autofree char *direction = g_settings_get_string(settings.get(), "sort-direction");
    g_assert_cmpstr(direction, ==, "ascending");
    g_assert_cmpuint(read_positions(settings.get()).at("file:///item").column, ==, 4);
    undo_latest(f.app.get());
    g_assert_true(read_positions(settings.get()).empty());
    undo_latest(f.app.get());
    g_autoptr(GVariant) disabled = g_action_get_state(toggle.get());
    g_assert_false(g_variant_get_boolean(disabled));
}

static void restored_position()
{
    Fixture f;
    auto settings = desktop_preferences();
    attach_undo_preferences(f.app.get(), settings.get());
    fs::create_directories(g_get_user_config_dir());
    std::ofstream(fs::path(g_get_user_config_dir()) / "user-dirs.dirs") << "XDG_DESKTOP_DIR=\"" << f.root.string() << "\"\n";
    g_reload_user_special_dirs_cache();
    ObjectPtr<GFile> desktop{g_file_new_for_path(f.root.c_str())};
    ObjectPtr<GFile> original{g_file_get_child(desktop.get(), "positioned")};
    g_autofree char *uri = g_file_get_uri(original.get());
    auto moved = f.file("elsewhere");
    std::ofstream(g_file_peek_path(original.get())) << "test";
    write_positions(settings.get(), {{uri, {4, 2}}, {"file:///other", {0, 0}}});
    auto step = begin_undo_step(f.app.get());
    g_assert_true(rename_no_replace(original.get(), moved.get(), nullptr));
    remember_file(step, moved.get(), original.get());
    write_positions(settings.get(), {{"file:///other", {3, 1}}});
    finish_undo_step(f.app.get(), step);
    undo_latest(f.app.get());
    wait_for([&]
             { return read_positions(settings.get()).contains(uri); });
    auto positions = read_positions(settings.get());
    g_assert_cmpuint(positions.at(uri).column, ==, 4);
    g_assert_cmpuint(positions.at(uri).row, ==, 2);
    g_assert_cmpuint(positions.at("file:///other").column, ==, 3);
}

int main(int argc, char **argv)
{
    g_setenv("GSETTINGS_BACKEND", "memory", true);
    g_setenv("GIO_USE_VFS", "local", true);
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/dev/null", true);
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/undo/mixed-history", mixed_history);
    g_test_add_func("/undo/partial-batch", partial_batch);
    g_test_add_func("/undo/collision-retry", collision_retry);
    g_test_add_func("/undo/original-folder-unavailable", original_folder_unavailable);
    g_test_add_func("/undo/numbered-restore", numbered_restore);
    g_test_add_func("/undo/numbered-folder", numbered_folder_restore);
    g_test_add_func("/undo/history-limit", history_limit);
    g_test_add_func("/undo/retry-at-history-limit", retry_at_history_limit);
    g_test_add_func("/undo/views", views);
    g_test_add_func("/undo/restored-position", restored_position);
    return g_test_run();
}
