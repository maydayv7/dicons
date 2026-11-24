#include "desktop_directory.hpp"
#include "icon_grid.hpp"
#include "inline_rename.hpp"
#include "preferences.hpp"
#include "selection_actions.hpp"
#include "trash_actions.hpp"
#include "undo.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static void wait_for(auto condition)
{
    const auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!condition())
    {
        g_assert_true(g_get_monotonic_time() < deadline);
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
}

struct Desktop
{
    fs::path root = fs::path(g_get_user_data_dir()) / "Desktop";
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    ObjectPtr<GListStore> items{g_list_store_new(G_TYPE_FILE_INFO)};
    ObjectPtr<GSettings> settings = desktop_preferences();
    GtkWindow *window;
    GtkGridView *grid;

    Desktop()
    {
        fs::create_directories(root);
        fs::create_directories(g_get_home_dir());
        g_assert_true(g_application_register(G_APPLICATION(app.get()), nullptr, nullptr));
        window = GTK_WINDOW(gtk_application_window_new(app.get()));
        auto *model = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(items.get())));
        grid = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(model), nullptr));
        gtk_window_set_child(window, GTK_WIDGET(grid));
        gtk_window_present(window);
    }
    ~Desktop()
    {
        if (window)
            gtk_window_destroy(window);
    }
    ObjectPtr<GFile> file(const char *name) { return ObjectPtr<GFile>{g_file_new_for_path((root / name).c_str())}; }
    void undo() { undo_latest(G_APPLICATION(app.get()), grid); }
    void wait_for_undo()
    {
        change_view(G_APPLICATION(app.get()), settings.get(), nullptr, [&]
                    { g_settings_set_int(settings.get(), "icon-size", 80); });
        undo_latest(G_APPLICATION(app.get()));
        wait_for([&]
                 { return g_settings_get_int(settings.get(), "icon-size") == 48; });
    }
    ObjectPtr<GtkWindow> dialog()
    {
        auto find = [&]() -> ObjectPtr<GtkWindow>
        {
            auto *windows = gtk_window_get_toplevels();
            for (guint i = 0; i < g_list_model_get_n_items(windows); ++i)
            {
                ObjectPtr<GtkWindow> current{GTK_WINDOW(g_list_model_get_item(windows, i))};
                if (gtk_window_get_transient_for(current.get()) == window)
                    return current;
            }
            return {};
        };
        wait_for([&]
                 { return bool(find()); });
        return find();
    }
};

static GtkWidget *trash_button(GtkWidget *widget)
{
    if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), "Move to Trash") == 0)
        return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto *button = trash_button(child))
            return button;
    return nullptr;
}

static void confirm(GtkWindow *dialog)
{
    auto *button = trash_button(GTK_WIDGET(dialog));
    g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked");
    wait_for([&]
             { return !gtk_widget_get_visible(GTK_WIDGET(dialog)); });
}

static void undo_copied_folders()
{
    Desktop d;
    auto step = begin_undo_step(G_APPLICATION(d.app.get()));
    for (auto name : {"file", "replace", "approve", "keep"})
    {
        if (g_str_equal(name, "file"))
            std::ofstream(d.root / name) << name;
        else
            fs::create_directory(d.root / name);
        remember_new_file(step, d.file(name).get(), FileOrigin::copied);
        if (!g_str_equal(name, "file"))
            std::ofstream(d.root / name / ".added") << name;
    }
    finish_undo_step(G_APPLICATION(d.app.get()), step);
    d.undo();
    auto keep = d.dialog();
    gtk_window_close(keep.get());
    confirm(d.dialog().get());
    auto pending = d.dialog();
    fs::rename(d.root / "replace", d.root / "original");
    fs::create_directory_symlink("original", d.root / "replace");
    confirm(pending.get());
    d.wait_for_undo();
    g_assert_false(fs::exists(d.root / "file"));
    g_assert_false(fs::exists(d.root / "approve"));
    g_assert_true(fs::exists(fs::path(g_get_user_data_dir()) / "Trash/files/approve/.added"));
    g_assert_true(fs::exists(d.root / "keep/.added"));
    g_assert_true(fs::is_symlink(d.root / "replace"));
    g_assert_true(fs::exists(d.root / "original/.added"));

    d.undo();
    pending = d.dialog();
    gtk_window_destroy(d.window);
    d.window = nullptr;
    d.wait_for_undo();
    g_assert_true(fs::exists(d.root / "keep/.added"));
}

static void queued_trash()
{
    Desktop d;
    for (auto name : {"first", "changed", "valid", "external", "last"})
        std::ofstream(d.root / name) << name;
    fs::create_symlink("missing", d.root / "link");
    for (auto name : {"changed", "link", "valid"})
    {
        auto file = d.file(name);
        ObjectPtr<GFileInfo> info{g_file_query_info(file.get(), drop_identity_attributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr)};
        cache_link_identity(info.get(), file.get());
        g_file_info_set_attribute_object(info.get(), "standard::file", G_OBJECT(file.get()));
        g_list_store_append(d.items.get(), info.get());
    }
    gtk_selection_model_select_all(gtk_grid_view_get_model(d.grid));
    trash_files(d.grid, {d.file("first").get()});
    trash_selection(d.grid);
    fs::rename(d.root / "changed", d.root / "old file");
    std::ofstream(d.root / "changed") << "replacement";
    fs::rename(d.root / "link", d.root / "old link");
    fs::create_symlink("missing", d.root / "link");
    bool finished = false;
    trash_files(d.grid, {d.file("external").get(), d.file("last").get()}, [&](bool success)
                { g_assert_false(success); finished = true; });
    fs::rename(d.root / "external", d.root / "old external");
    std::ofstream(d.root / "external") << "replacement";
    wait_for([&]
             { return finished; });
    g_assert_true(fs::exists(d.root / "changed"));
    g_assert_true(fs::is_symlink(d.root / "link"));
    g_assert_true(fs::is_regular_file(d.root / "external"));
    g_assert_false(fs::exists(d.root / "valid"));
    d.undo();
    d.undo();
    d.wait_for_undo();
    g_assert_true(fs::exists(d.root / "valid"));
    g_assert_true(fs::exists(d.root / "last"));
    g_assert_false(fs::exists(d.root / "first"));
}

static void inline_replacement()
{
    Desktop d;
    std::ofstream(d.root / "note") << "original";
    g_settings_set_boolean(d.settings.get(), "show-trash", false);
    auto folder = d.file(".");
    DesktopDirectory directory(folder.get(), d.app.get(), d.settings.get());
    auto *grid = create_icon_grid(directory, d.app.get());
    auto *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    attach_grid_viewport(scroll, grid);
    gtk_window_set_default_size(d.window, 480, 320);
    gtk_window_set_child(d.window, GTK_WIDGET(scroll));
    attach_inline_rename(d.window, grid);
    GtkListItem *tile = nullptr;
    auto *model = gtk_grid_view_get_model(grid);
    auto note = d.file("note");
    wait_for([&]
             {
        for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(model)); ++i)
        {
            ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(model), i))};
            if (item_file(info.get()) && g_file_equal(item_file(info.get()), note.get()))
                tile = bound_item(grid, info.get());
            if (tile)
                return true;
        }
        return false; });
    gtk_selection_model_select_item(model, gtk_list_item_get_position(tile), true);
    rename_selection(grid);
    auto *entry = gtk_widget_get_last_child(tile_content(tile));
    g_assert_true(GTK_IS_ENTRY(entry) && gtk_widget_get_visible(entry));
    gtk_editable_set_text(GTK_EDITABLE(entry), "renamed");
    fs::rename(d.root / "note", d.root / "away");
    std::ofstream(d.root / "note") << "replacement";
    g_signal_emit_by_name(entry, "activate");

    // The worker retains the grid until its completion callback runs.
    gpointer live = grid;
    g_object_add_weak_pointer(G_OBJECT(grid), &live);
    gtk_window_set_child(d.window, nullptr);
    wait_for([&]
             { return live == nullptr; });
    g_assert_false(fs::exists(d.root / "renamed"));
    g_assert_true(fs::exists(d.root / "away"));
    g_assert_true(fs::exists(d.root / "note"));
}

int main(int argc, char **argv)
{
    if (!g_getenv("DICONS_TEST_DISPLAY"))
    {
        g_print("Set DICONS_TEST_DISPLAY on an isolated GTK display to run the file-action tests.\n");
        return 77;
    }
    g_setenv("GSETTINGS_BACKEND", "memory", true);
    g_setenv("GIO_USE_VFS", "local", true);
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/dev/null", true);
    gtk_init();
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/file-actions/undo-copied-folders", undo_copied_folders);
    g_test_add_func("/file-actions/queued-trash", queued_trash);
    g_test_add_func("/file-actions/inline-replacement", inline_replacement);
    return g_test_run();
}
