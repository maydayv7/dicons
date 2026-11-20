#include "desktop_directory.hpp"
#include "preferences.hpp"
#include "grid_layout.hpp"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

static ObjectPtr<GSettings> test_preferences()
{
    ObjectPtr<GSettingsBackend> backend{g_memory_settings_backend_new()};
    return desktop_preferences(backend.get());
}

static void settings_without_dconf()
{
    if (!g_test_subprocess())
    {
        g_test_trap_subprocess(nullptr, 5 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
        g_test_trap_assert_passed();
        return;
    }
    g_unsetenv("GSETTINGS_BACKEND");
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/dicons-test-bus", true);
    auto path = fs::path(g_get_user_config_dir()) / "dicons" / "settings.ini";
    auto settings = desktop_preferences();
    g_assert_true(g_settings_set_int(settings.get(), "icon-size", 72));
    g_settings_sync();
    g_autoptr(GKeyFile) ini = g_key_file_new();
    g_assert_true(g_key_file_load_from_file(ini, path.c_str(), G_KEY_FILE_NONE, nullptr));
    g_assert_cmpint(g_key_file_get_integer(ini, "desktop", "icon-size", nullptr), ==, 72);
    settings.reset();

    std::ofstream(path) << "[desktop]\nicon-size=32\nshow-trash=false\nsort-field='size'\n";
    settings = desktop_preferences();
    g_assert_cmpint(g_settings_get_int(settings.get(), "icon-size"), ==, 32);
    g_assert_false(g_settings_get_boolean(settings.get(), "show-trash"));
    g_autofree char *field = g_settings_get_string(settings.get(), "sort-field");
    g_assert_cmpstr(field, ==, "size");
}

static guint position_of(GListModel *model, std::string_view name)
{
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(model, i))};
        if (g_file_info_get_name(info.get()) == name)
            return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

static bool wait_for(auto predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do
    {
        while (g_main_context_iteration(nullptr, false))
        {
        }
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static bool checked(GAction *action)
{
    g_autoptr(GVariant) state = g_action_get_state(action);
    return g_variant_get_boolean(state);
}

static void stored_preferences()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / "older.txt") << "old";
    std::ofstream(root / "newer.txt") << "new";
    fs::last_write_time(root / "older.txt", fs::file_time_type::clock::now() - 2h);
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    ObjectPtr<GSettingsBackend> backend{g_memory_settings_backend_new()};
    auto external = desktop_preferences(backend.get());
    g_settings_set_boolean(external.get(), "show-trash", false);
    g_settings_set_boolean(external.get(), "show-thumbnails", false);

    {
        auto preferences = desktop_preferences(backend.get());
        DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
        auto *model = directory.items.get();
        g_assert_false(checked(directory.show_thumbnails.get()));
        g_assert_true(wait_for([&]
                               { return g_list_model_get_n_items(model) == 2; }));
        g_settings_set_string(external.get(), "sort-field", "modified");
        g_assert_true(wait_for([&]
                               { return position_of(model, "older.txt") == 0; }));
        g_assert_null(g_settings_get_user_value(external.get(), "sort-direction"));
        g_settings_set_boolean(external.get(), "show-thumbnails", true);
        g_assert_true(wait_for([&]
                               { return checked(directory.show_thumbnails.get()); }));
        g_action_activate(directory.trash.visible.get(), nullptr);
        g_assert_true(g_settings_get_boolean(external.get(), "show-trash"));
        g_action_activate(G_ACTION(directory.show_hidden.get()), nullptr);

        auto *actions = G_ACTION_GROUP(directory.sorting.actions.get());
        g_action_group_activate_action(actions, "field", g_variant_new_string("name"));
        g_action_group_activate_action(actions, "field", g_variant_new_string("modified"));
        g_assert_true(wait_for([&]
                               { return position_of(model, "newer.txt") == 1; }));
        g_action_group_activate_action(actions, "sort-direction", g_variant_new_string("ascending"));
    }
    auto preferences = desktop_preferences(backend.get());
    DesktopDirectory reopened{folder.get(), app.get(), preferences.get()};
    g_assert_true(checked(reopened.trash.visible.get()));
    g_assert_true(checked(reopened.show_thumbnails.get()));
    g_assert_false(checked(G_ACTION(reopened.show_hidden.get())));
    g_assert_true(wait_for([&]
                           { return position_of(reopened.items.get(), "older.txt") == 1; }));
}

static void manual_layout()
{
    auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / "a");
    std::ofstream(root / "b");
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    GridLayout layout(directory.items.get(), preferences.get());
    layout.resize(5, 4);
    auto *slots = G_LIST_MODEL(layout.slots.get());
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(slots) == 3; }));
    const auto uri = [&](const char *name)
    {
        ObjectPtr<GFile> file{g_file_get_child(folder.get(), name)};
        g_autofree char *text = g_file_get_uri(file.get());
        return std::string{text};
    };
    auto at = [&](guint i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(slots, i))};
        auto *name = info && item_file(info.get()) ? g_file_info_get_name(info.get()) : nullptr;
        return std::string{name ? name : ""};
    };

    ObjectPtr<GtkMultiSelection> selection{gtk_multi_selection_new(g_object_ref(slots))};
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection.get()), 1, true);
    g_assert_true(layout.move({uri("a"), uri("b")}, 12));
    g_assert_true(at(0) == "Trash" && at(1).empty() && at(12) == "a" && at(13) == "b");
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection.get()), 12));
    g_assert_false(layout.move({uri("a")}, 13));
    g_assert_false(layout.move({uri("a"), uri("b")}, 14));
    g_assert_true(at(12) == "a" && at(13) == "b");

    fs::rename(root / "a", root / "renamed");
    g_assert_true(wait_for([&]
                           { return at(12) == "renamed"; }));
    GridLayout mirror(directory.items.get(), preferences.get());
    mirror.resize(5, 4);
    ObjectPtr<GFileInfo> mirrored{G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(mirror.slots.get()), 12))};
    g_assert_cmpstr(g_file_info_get_name(mirrored.get()), ==, "renamed");
    layout.resize(2, 2);
    g_assert_cmpuint(g_list_model_get_n_items(slots), ==, 3);
    layout.resize(5, 4);
    g_assert_true(at(12) == "renamed" && at(13) == "b");
    fs::remove(root / "renamed");
    g_assert_true(wait_for([&]
                           { return at(12).empty(); }));
    g_assert_false(read_positions(preferences.get()).contains(uri("renamed")));
    g_action_group_activate_action(G_ACTION_GROUP(directory.sorting.actions.get()), "field", g_variant_new_string("name"));
    g_assert_true(at(0) == "Trash" && at(1) == "b");
    g_assert_cmpuint(g_list_model_get_n_items(slots), ==, 2);
}

static void pinned_trash()
{
    const fs::path data = g_get_user_data_dir();
    const auto folder = data / "Desktop";
    fs::create_directories(folder / "Trash");
    ObjectPtr<GFile> desktop{g_file_new_for_path(folder.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{desktop.get(), app.get(), preferences.get()};
    auto *model = directory.items.get();
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(model) == 2; }));
    ObjectPtr<GFileInfo> pinned{G_FILE_INFO(g_list_model_get_item(model, 0))};
    g_assert_true(is_trash_icon(pinned.get()));
    ObjectPtr<GtkMultiSelection> selection{gtk_multi_selection_new(g_object_ref(model))};
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(selection.get()), 1, true);
    auto *sort = G_ACTION_GROUP(directory.sorting.actions.get());
    g_action_group_activate_action(sort, "sort-direction", g_variant_new_string("descending"));
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection.get()), 1));
    g_action_activate(G_ACTION(directory.trash.visible.get()), nullptr);
    g_assert_cmpuint(g_list_model_get_n_items(model), ==, 1);
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection.get()), 0));
    g_action_activate(G_ACTION(directory.trash.visible.get()), nullptr);
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(selection.get()), 1));

    const auto files = data / "Trash/files";
    fs::create_directories(files);
    std::ofstream(files / "from outside") << "example";
    auto full = [&]
    { return g_file_info_get_attribute_boolean(pinned.get(), "dicons::full"); };
    g_assert_true(wait_for(full));
    fs::remove(files / "from outside");
    g_assert_true(wait_for([&]
                           { return !full(); }));
}

static void atomic_saves()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / "selected.txt") << "keep selected";
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    const auto selection = [&directory]
    {
        return ObjectPtr<GtkMultiSelection>{gtk_multi_selection_new(g_object_ref(directory.items.get()))};
    };
    const auto first = selection();
    const auto second = selection();
    auto *left = G_LIST_MODEL(first.get());
    auto *right = G_LIST_MODEL(second.get());

    // Race both the initial enumeration and later metadata lookups
    std::ofstream(root / "initial.tmp") << "initial";
    fs::rename(root / "initial.tmp", root / "edited.txt");
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(left) == 2 && position_of(left, "selected.txt") != GTK_INVALID_LIST_POSITION &&
                                    position_of(left, "edited.txt") != GTK_INVALID_LIST_POSITION; }));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(first.get()), position_of(left, "selected.txt"), true);

    for (int i = 0; i < 32; ++i)
    {
        std::ofstream(root / "editor.tmp") << i;
        fs::rename(root / "editor.tmp", root / "edited.txt");
    }
    fs::remove(root / "edited.txt");
    std::ofstream(root / "after.txt") << "later change";
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(left) == 2 && position_of(left, "after.txt") != GTK_INVALID_LIST_POSITION &&
                                    position_of(right, "after.txt") != GTK_INVALID_LIST_POSITION; }));
    fs::remove(root / "after.txt");
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(left) == 1 && g_list_model_get_n_items(right) == 1 &&
                                    position_of(left, "selected.txt") == 0 && position_of(right, "selected.txt") == 0; }));
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(first.get()), 0));
    g_assert_false(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(second.get()), 0));
}

static void desktop_folder_events()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / "first") << "first";
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    auto *items = directory.items.get();
    g_assert_true(wait_for([&]
                           { return position_of(items, "first") != GTK_INVALID_LIST_POSITION; }));

    fs::permissions(root, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
    std::ofstream(root / "second") << "second";
    g_assert_true(wait_for([&]
                           { return position_of(items, "second") != GTK_INVALID_LIST_POSITION; }));
    g_assert_cmpuint(g_list_model_get_n_items(items), ==, 2);
    g_assert_cmpuint(position_of(items, "Desktop"), ==, GTK_INVALID_LIST_POSITION);
}

static void hidden_items()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / ".hidden") << "hidden";
    std::ofstream(root / "visible") << "visible";
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    ObjectPtr<GFile> staging{g_file_get_child(folder.get(), ".working")};
    g_hash_table_add(directory.staging.get(), g_object_ref(staging.get()));
    fs::create_directory(root / ".working");

    auto *model = directory.items.get();
    g_assert_true(wait_for([&]
                           { return position_of(model, "visible") != GTK_INVALID_LIST_POSITION; }));
    g_assert_cmpuint(g_list_model_get_n_items(model), ==, 1);
    g_action_activate(G_ACTION(directory.show_hidden.get()), nullptr);
    g_assert_true(wait_for([&]
                           { return position_of(model, ".hidden") != GTK_INVALID_LIST_POSITION; }));
    g_assert_cmpuint(position_of(model, ".working"), ==, GTK_INVALID_LIST_POSITION);
    fs::rename(root / "visible", root / ".renamed");
    g_assert_true(wait_for([&]
                           { return position_of(model, ".renamed") != GTK_INVALID_LIST_POSITION; }));
    g_action_activate(G_ACTION(directory.show_hidden.get()), nullptr);
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(model) == 0; }));
}

static void sorted_directory()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root / "Zoo");
    std::ofstream(root / "File10.txt") << "a";
    std::ofstream(root / "file2.txt") << "bb";
    std::ofstream(root / "photo.png") << "\x89PNG\r\n\x1a\n";
    const auto oldest = fs::file_time_type::clock::now() - 2h;
    fs::last_write_time(root / "File10.txt", oldest);
    fs::last_write_time(root / "file2.txt", oldest);
    fs::last_write_time(root / "photo.png", oldest + 1h);

    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    auto *model = directory.items.get();
    const auto expect = [&](std::initializer_list<const char *> names)
    {
        g_assert_true(wait_for([&]
                               {
            guint i = 0;
            for (auto name : names)
                if (position_of(model, name) != i++)
                    return false;
            return g_list_model_get_n_items(model) == names.size(); }));
    };
    auto *actions = G_ACTION_GROUP(directory.sorting.actions.get());
    const auto choose = [&](const char *action, const char *value)
    { g_action_group_activate_action(actions, action, g_variant_new_string(value)); };

    expect({"Zoo", "file2.txt", "File10.txt", "photo.png"});
    ObjectPtr<GtkMultiSelection> left{gtk_multi_selection_new(g_object_ref(model))};
    ObjectPtr<GtkMultiSelection> right{gtk_multi_selection_new(g_object_ref(model))};
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(left.get()), 1, true);
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(right.get()), 2, true);

    choose("sort-direction", "descending");
    expect({"Zoo", "photo.png", "File10.txt", "file2.txt"});
    g_action_group_activate_action(actions, "folders-first", nullptr);
    choose("sort-direction", "ascending");
    expect({"file2.txt", "File10.txt", "photo.png", "Zoo"});
    g_action_group_activate_action(actions, "folders-first", nullptr);
    choose("field", "modified");
    expect({"Zoo", "photo.png", "file2.txt", "File10.txt"});
    choose("sort-direction", "ascending");
    expect({"Zoo", "file2.txt", "File10.txt", "photo.png"});
    std::ofstream(root / "file2.txt", std::ios::app) << "new";
    expect({"Zoo", "File10.txt", "photo.png", "file2.txt"});
    choose("field", "type");
    expect({"Zoo", "photo.png", "file2.txt", "File10.txt"});
    choose("field", "size");
    expect({"Zoo", "File10.txt", "file2.txt", "photo.png"});
    std::ofstream(root / "File10.txt", std::ios::app) << "now the largest";
    expect({"Zoo", "file2.txt", "photo.png", "File10.txt"});
    choose("sort-direction", "descending");
    expect({"Zoo", "File10.txt", "photo.png", "file2.txt"});
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(left.get()), 3));
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(right.get()), 1));

    std::ofstream(root / "new.txt") << "123456";
    expect({"Zoo", "File10.txt", "photo.png", "new.txt", "file2.txt"});
    fs::rename(root / "new.txt", root / "renamed.txt");
    expect({"Zoo", "File10.txt", "photo.png", "renamed.txt", "file2.txt"});
}

static void launcher_order()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    const auto launcher = root / "zz.desktop";
    auto write_launcher = [&](const char *label)
    { std::ofstream(launcher) << "[Desktop Entry]\nType=Application\nExec=true\nName=" << label << '\n'; };
    write_launcher("Alpha");
    std::ofstream(root / "middle.txt") << "middle";
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    auto *model = directory.items.get();
    g_assert_true(wait_for([&]
                           { return position_of(model, "middle.txt") == 1; }));
    write_launcher("Zulu");
    g_assert_true(wait_for([&]
                           { return position_of(model, "middle.txt") == 0; }));
    write_launcher("Alpha");
    g_assert_true(wait_for([&]
                           { return position_of(model, "middle.txt") == 1; }));
    std::ofstream(launcher) << "invalid desktop entry";
    g_assert_true(wait_for([&]
                           { return position_of(model, "middle.txt") == 0; }));
}

static void symlink_targets()
{
    const auto base = fs::path(g_get_user_data_dir());
    const auto root = base / "Desktop";
    fs::create_directories(root);
    std::ofstream(base / "target.txt") << "abc";
    fs::create_symlink(base / "target.txt", base / "hop");
    fs::create_symlink("../hop", root / "shortcut");

    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    auto expect = [&](GFileType type, goffset size = -1)
    {
        g_assert_true(wait_for([&]
                               {
            ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(directory.items.get(), 0))};
            return info && g_file_info_has_attribute(info.get(), "dicons::link-targets") &&
                   g_file_info_get_is_symlink(info.get()) && g_file_info_get_file_type(info.get()) == type &&
                   (size == -1 || g_file_info_get_size(info.get()) == size); }));
    };
    expect(G_FILE_TYPE_REGULAR, 3);
    std::ofstream(base / "target.txt", std::ios::app) << "def";
    expect(G_FILE_TYPE_REGULAR, 6);
    fs::remove(base / "target.txt");
    expect(G_FILE_TYPE_SYMBOLIC_LINK);
    fs::create_directory(base / "target.txt");
    expect(G_FILE_TYPE_DIRECTORY);
    fs::remove(base / "hop");
    expect(G_FILE_TYPE_SYMBOLIC_LINK);
    std::ofstream(base / "hop") << "new";
    expect(G_FILE_TYPE_REGULAR, 3);
    std::ofstream(base / "app.desktop") << "[Desktop Entry]\nType=Application\nName=Calendar\nIcon=office-calendar\nExec=true\n";
    fs::remove(base / "hop");
    fs::create_symlink("app.desktop", base / "hop");
    ObjectPtr<GIcon> expected{g_themed_icon_new("office-calendar")};
    g_assert_true(wait_for([&]
                           {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(directory.items.get(), 0))};
        return info && g_file_info_get_icon(info.get()) && g_icon_equal(g_file_info_get_icon(info.get()), expected.get()); }));
}

static void directory_burst()
{
    constexpr guint initial = 4000;
    constexpr guint burst = 500;
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    const auto path = [&](guint i)
    { return root / (std::to_string(i) + ".txt"); };
    for (guint i = 0; i < initial; ++i)
        std::ofstream{path(i)};

    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    g_test_timer_start();
    auto preferences = test_preferences();
    DesktopDirectory directory{folder.get(), app.get(), preferences.get()};
    g_action_change_state(directory.trash.visible.get(), g_variant_new_boolean(false));
    const auto await_count = [&](guint count)
    {
        bool expired = false;
        guint timer = g_timeout_add_seconds(60, [](gpointer flag) -> gboolean
                                            {
            *static_cast<bool *>(flag) = true;
            return G_SOURCE_CONTINUE; }, &expired);
        while (!expired && g_list_model_get_n_items(directory.items.get()) != count)
            g_main_context_iteration(nullptr, true);
        g_source_remove(timer);
        g_assert_false(expired);
    };
    await_count(initial);
    g_test_message("Scan %u files: %.3f s", initial, g_test_timer_elapsed());

    g_test_timer_start();
    for (guint i = initial; i < initial + burst; ++i)
        std::ofstream{path(i)};
    await_count(initial + burst);
    g_test_message("Create %u files and update model: %.3f s", burst, g_test_timer_elapsed());

    g_test_timer_start();
    for (guint i = 0; i < burst; ++i)
        fs::remove(path(i * (initial / burst)));
    await_count(initial);
    g_test_message("Delete %u files and update model: %.3f s", burst, g_test_timer_elapsed());
}

int main(int argc, char **argv)
{
    g_setenv("GIO_USE_VFS", "local", true);
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/directory/stored-preferences", stored_preferences);
    g_test_add_func("/directory/settings-without-dconf", settings_without_dconf);
    g_test_add_func("/directory/manual-layout", manual_layout);
    g_test_add_func("/directory/pinned-trash", pinned_trash);
    g_test_add_func("/directory/atomic-saves", atomic_saves);
    g_test_add_func("/directory/desktop-folder-events", desktop_folder_events);
    g_test_add_func("/directory/hidden-items", hidden_items);
    g_test_add_func("/directory/sorted-directory", sorted_directory);
    g_test_add_func("/directory/launcher-order", launcher_order);
    g_test_add_func("/directory/symlink-targets", symlink_targets);
    if (g_test_perf())
        g_test_add_func("/directory/burst", directory_burst);
    return g_test_run();
}
