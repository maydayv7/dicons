#include "desktop_directory.hpp"
#include "desktop_windows.hpp"
#include "context_menu.hpp"
#include "clipboard.hpp"
#include "grid_layout.hpp"
#include "icon_grid.hpp"
#include "inline_rename.hpp"
#include "preferences.hpp"
#include "selection_actions.hpp"
#include "trash_actions.hpp"
#include "transfer.hpp"
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

static GtkWidget *find_button(GtkWidget *widget, const char *label)
{
    if (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
        return widget;
    for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
        if (auto *button = find_button(child, label))
            return button;
    return nullptr;
}

static void confirm(GtkWindow *dialog)
{
    auto *button = find_button(GTK_WIDGET(dialog), "Move to Trash");
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

static GtkGridView *mount_grid(Desktop &desktop, DesktopDirectory &directory, GtkFixed *fixed, int width, int top)
{
    auto *grid = create_icon_grid(directory, desktop.app.get());
    auto *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
    gtk_widget_set_size_request(GTK_WIDGET(scroll), width, 340);
    gtk_fixed_put(fixed, GTK_WIDGET(scroll), 0, top);
    attach_grid_viewport(scroll, grid);
    return grid;
}

static void cut_feedback()
{
    Desktop d;
    g_settings_reset(d.settings.get(), "icon-positions");
    fs::create_directory(d.root / "Folder");
    fs::create_directory_symlink("Folder", d.root / "Link");
    for (auto name : {"a", "b", "Folder/b"})
        std::ofstream(d.root / name) << name;
    g_settings_set_boolean(d.settings.get(), "show-trash", false);
    auto folder = d.file(".");
    DesktopDirectory directory(folder.get(), d.app.get(), d.settings.get());
    auto *fixed = GTK_FIXED(gtk_fixed_new());
    gtk_window_set_child(d.window, GTK_WIDGET(fixed));
    auto *grid = mount_grid(d, directory, fixed, 620, 0);
    auto *mirror = mount_grid(d, directory, fixed, 320, 340);
    transfer_attach(d.window, grid, folder.get(), directory.staging.get());
    auto *selection = gtk_grid_view_get_model(grid);
    auto tile = [&](GtkGridView *view, const char *name) -> GtkListItem *
    {
        for (guint i = 0; i < g_list_model_get_n_items(directory.items.get()); ++i)
        {
            ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(directory.items.get(), i))};
            if (g_strcmp0(g_file_info_get_name(info.get()), name) == 0)
                return bound_item(view, info.get());
        }
        return nullptr;
    };
    auto marked = [&](GtkGridView *view, const char *name)
    {
        auto *item = tile(view, name);
        return item && gtk_widget_has_css_class(tile_content(item), "cut");
    };
    auto select = [&](const char *name, bool only = true)
    {
        gtk_selection_model_select_item(selection, gtk_list_item_get_position(tile(grid, name)), only);
    };
    wait_for([&]
             { return tile(grid, "a") && tile(mirror, "b") && tile(grid, "Link"); });
    select("a");
    select("b", false);
    copy_selection(grid, true);
    g_assert_true(marked(grid, "a") && marked(mirror, "b"));
    select("Link");
    paste_selection(grid, true);
    auto collision = d.dialog();
    auto *skip = find_button(GTK_WIDGET(collision.get()), "Skip");
    g_assert_nonnull(skip);
    g_signal_emit_by_name(skip, "clicked");
    auto *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(grid));
    wait_for([&]
             { return !g_object_get_data(G_OBJECT(clipboard), "dicons-paste") && !tile(grid, "a"); });
    g_assert_true(fs::exists(d.root / "Folder/a"));
    g_assert_true(marked(grid, "b") && marked(mirror, "b"));
    undo_latest(G_APPLICATION(d.app.get()), grid);
    wait_for([&]
             { return tile(grid, "a") && tile(mirror, "a"); });
    g_assert_false(marked(grid, "a"));
    g_assert_true(marked(grid, "b"));
    g_assert_true(read_positions(d.settings.get()).empty());
    auto replacement = d.root.parent_path() / "replacement";
    std::ofstream(replacement) << "b";
    fs::rename(replacement, d.root / "b");
    wait_for([&]
             { return !marked(grid, "b") && !marked(mirror, "b"); });
    select("Link");
    copy_selection(grid, true);
    g_assert_true(marked(grid, "Link") && marked(mirror, "Link"));
    auto *info = G_FILE_INFO(gtk_list_item_get_item(tile(grid, "Link")));
    highlight_drop_target(grid, info);
    g_assert_true(gtk_widget_has_css_class(tile_content(tile(grid, "Link")), "drop-target"));
    g_assert_true(marked(grid, "Link") && gtk_list_item_get_selected(tile(grid, "Link")));
    highlight_drop_target(grid, nullptr);
    g_assert_false(gtk_widget_has_css_class(tile_content(tile(grid, "Link")), "drop-target"));
    g_assert_true(marked(grid, "Link"));
    copy_selection(grid, false);
    g_assert_false(marked(mirror, "Link"));
    paste_selection(grid, false);
    wait_for([&]
             { return tile(grid, "Link (1)") != nullptr; });
    g_assert_true(fs::is_symlink(d.root / "Link (1)"));
    g_assert_true(read_positions(d.settings.get()).empty());
    select("Link");
    copy_selection(grid, true);
    gdk_clipboard_set_text(clipboard, "another owner");
    g_assert_false(marked(grid, "Link") || marked(mirror, "Link"));
    select("b");
    copy_selection(grid, true);
    select("Folder");
    paste_selection(grid, true);
    collision = d.dialog();
    select("a");
    copy_selection(grid, true);
    g_signal_emit_by_name(find_button(GTK_WIDGET(collision.get()), "Skip"), "clicked");
    wait_for([&]
             { return !gtk_widget_get_visible(GTK_WIDGET(collision.get())); });
    g_assert_true(marked(grid, "a") && !marked(grid, "b"));
    auto latest = retain(G_FILE_INFO(gtk_list_item_get_item(tile(grid, "a"))));
    gpointer alive = grid;
    g_object_add_weak_pointer(G_OBJECT(grid), &alive);
    gtk_window_set_child(d.window, nullptr);
    wait_for([&]
             { return !alive; });
    g_assert_true(clipboard_item_is_cut(clipboard, latest.get()));
    gdk_clipboard_set_content(clipboard, nullptr);
}

static void metadata_refresh()
{
    Desktop desktop;
    auto launcher = desktop.root / "mail.desktop";
    auto set_label = [&](const char *label)
    { std::ofstream(launcher) << "[Desktop Entry]\nType=Application\nExec=true\nName=" << label << '\n'; };
    set_label("Mail");
    std::ofstream(desktop.root / "unrelated.txt") << "abc";
    g_settings_set_boolean(desktop.settings.get(), "show-trash", false);
    g_settings_set_boolean(desktop.settings.get(), "show-thumbnails", false);
    DesktopDirectory directory(desktop.file(".").get(), desktop.app.get(), desktop.settings.get());
    auto *fixed = GTK_FIXED(gtk_fixed_new());
    gtk_window_set_child(desktop.window, GTK_WIDGET(fixed));
    auto *left = mount_grid(desktop, directory, fixed, 621, 0);
    auto *right = mount_grid(desktop, directory, fixed, 320, 340);
    attach_inline_rename(desktop.window, left);
    auto file = desktop.file("mail.desktop");
    g_autofree char *uri = g_file_get_uri(file.get());
    write_positions(desktop.settings.get(), {{uri, {2, 1}}});
    ObjectPtr<GFileInfo> info;
    wait_for([&]
             {
        info.reset(G_FILE_INFO(g_list_model_get_item(directory.items.get(), 0)));
        return info && g_strcmp0(item_label(info.get()), "Mail") == 0 &&
               bound_item(left, info.get()) && bound_item(right, info.get()) &&
               gtk_list_item_get_position(bound_item(left, info.get())) == gtk_grid_view_get_min_columns(left) + 2; });
    auto *tile = bound_item(left, info.get());
    gtk_selection_model_select_item(gtk_grid_view_get_model(left), gtk_list_item_get_position(tile), true);
    rename_selection(left);
    auto *entry = gtk_widget_get_last_child(tile_content(tile));
    g_assert_true(GTK_IS_ENTRY(entry) && gtk_widget_get_visible(entry));
    gtk_editable_set_text(GTK_EDITABLE(entry), "my draft");

    guint changes = 0, redraws = 0;
    g_signal_connect(directory.items.get(), "items-changed", G_CALLBACK(+[](GListModel *, guint, guint, guint, guint *n)
                                                                        { ++*n; }),
                     &changes);
    g_signal_connect(directory.item_updates.get(), "item-updated", G_CALLBACK(+[](DiconsItemUpdates *, GFileInfo *, guint *n)
                                                                              { ++*n; }),
                     &redraws);
    auto label = [&](GtkGridView *grid)
    {
        auto *box = tile_content(bound_item(grid, info.get()));
        return gtk_label_get_text(GTK_LABEL(gtk_widget_get_next_sibling(gtk_widget_get_first_child(box))));
    };
    set_label("MAIL");
    wait_for([&]
             { return g_str_equal(label(left), "MAIL") && g_str_equal(label(right), "MAIL"); });
    std::ofstream(desktop.root / "unrelated.txt") << "longer";
    wait_for([&]
             {
        ObjectPtr<GFileInfo> other{G_FILE_INFO(g_list_model_get_item(directory.items.get(), 1))};
        return other && g_file_info_get_size(other.get()) == 6; });
    g_assert_cmpuint(changes, ==, 0);
    g_assert_cmpuint(redraws, ==, 1);
    g_assert_true(bound_item(left, info.get()) == tile && gtk_list_item_get_selected(tile));
    g_assert_false(gtk_list_item_get_selected(bound_item(right, info.get())));
    g_assert_true(gtk_widget_get_visible(entry));
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(entry)), ==, "my draft");
    g_signal_handlers_disconnect_by_data(directory.items.get(), &changes);
    g_signal_handlers_disconnect_by_data(directory.item_updates.get(), &redraws);
    gtk_window_set_child(desktop.window, nullptr);
}

static void grid_density()
{
    Desktop desktop;
    std::ofstream(desktop.root / "placed.txt") << "text";
    g_settings_set_boolean(desktop.settings.get(), "show-trash", false);
    g_settings_set_boolean(desktop.settings.get(), "show-thumbnails", false);
    DesktopDirectory directory(desktop.file(".").get(), desktop.app.get(), desktop.settings.get());
    gtk_widget_add_css_class(GTK_WIDGET(desktop.window), "dicons");
    auto *overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_window_set_child(desktop.window, GTK_WIDGET(overlay));
    auto *fixed = GTK_FIXED(gtk_fixed_new());
    gtk_overlay_set_child(overlay, GTK_WIDGET(fixed));
    constexpr int width = 621;
    auto *grid = mount_grid(desktop, directory, fixed, width, 0);
    auto *narrow = mount_grid(desktop, directory, fixed, 60, 340);
    attach_context_menu(desktop.window, grid, directory);
    auto file = desktop.file("placed.txt");
    g_autofree char *uri = g_file_get_uri(file.get());
    write_positions(desktop.settings.get(), {{uri, {7, 1}}});
    ObjectPtr<GFileInfo> info;
    wait_for([&]
             {
        info.reset(G_FILE_INFO(g_list_model_get_item(directory.items.get(), 0)));
        return info && bound_item(grid, info.get()) && bound_item(narrow, info.get()) &&
               gtk_grid_view_get_min_columns(grid) == width / 72; });
    auto *widget = GTK_WIDGET(grid);
    g_assert_cmpint(gtk_widget_get_width(widget), ==, width);
    auto check = [&](int columns, int position)
    {
        auto *item = bound_item(grid, info.get());
        g_assert_nonnull(item);
        auto *cell = gtk_list_item_get_child(item);
        int height;
        gtk_widget_get_size_request(cell, nullptr, &height);
        graphene_rect_t bounds;
        wait_for([&]
                 { return gtk_widget_compute_bounds(cell, widget, &bounds) && int(bounds.size.height) == height &&
                          int(bounds.origin.y) == position / columns * height &&
                          grid_position_at(grid, bounds.origin.x + bounds.size.width / 2, bounds.origin.y + height / 2) == guint(position); });
        g_assert_cmpuint(gtk_grid_view_get_min_columns(grid), ==, columns);
        g_assert_cmpuint(gtk_list_item_get_position(item), ==, position);
        g_assert_cmpuint(gtk_list_item_get_position(bound_item(narrow, info.get())), ==, 0);
        auto saved = read_positions(desktop.settings.get()).at(uri);
        g_assert_cmpuint(saved.column, ==, 7);
        g_assert_cmpuint(saved.row, ==, 1);
        auto *icon = gtk_widget_get_first_child(tile_content(item));
        auto *base = GTK_IMAGE(gtk_overlay_get_child(GTK_OVERLAY(icon)));
        g_assert_cmpint(gtk_image_get_pixel_size(base), ==, g_settings_get_int(desktop.settings.get(), "icon-size"));
        guint row = std::min(2, 340 / height - 1);
        int boundary = (width + columns - 1) / columns;
        bool rtl = gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL;
        auto slot = [&](int x)
        { return grid_position_at(grid, rtl ? width - x - 1 : x, row * height + 1); };
        g_assert_cmpuint(slot(boundary - 1), ==, row * columns);
        g_assert_cmpuint(slot(boundary), ==, row * columns + 1);
        g_assert_cmpuint(slot(width - 1), ==, (row + 1) * columns - 1);
        return height;
    };
    auto *menu = gtk_widget_get_last_child(GTK_WIDGET(overlay));
    int normal = check(8, 15);
    g_assert_true(gtk_widget_activate_action(menu, "desktop.grid-density", "s", "compact"));
    int compact = check(10, 17);
    g_assert_cmpint(compact, <, normal);
    g_assert_true(gtk_widget_activate_action(menu, "desktop.grid-density", "s", "spacious"));
    g_assert_cmpint(check(7, 0), >, normal);
    undo_latest(G_APPLICATION(desktop.app.get()), grid);
    g_assert_cmpint(check(10, 17), ==, compact);
    gtk_widget_set_direction(widget, GTK_TEXT_DIR_RTL);
    g_settings_set_int(desktop.settings.get(), "icon-size", 24);
    check(11, 18);
    g_settings_set_int(desktop.settings.get(), "icon-size", 128);
    check(4, 0);
    g_assert_cmpuint(gtk_grid_view_get_min_columns(narrow), ==, 1);
    g_assert_cmpuint(grid_position_at(grid, -1, 0), ==, GTK_INVALID_LIST_POSITION);
    g_assert_cmpuint(grid_position_at(grid, width, 0), ==, GTK_INVALID_LIST_POSITION);
    g_assert_cmpuint(grid_position_at(grid, 0, gtk_widget_get_height(widget)), ==, GTK_INVALID_LIST_POSITION);

    for (int i = 0; i < 17; ++i)
        std::ofstream{desktop.root / std::to_string(i)};
    auto *slots = G_LIST_MODEL(gtk_grid_view_get_model(grid));
    wait_for([&]
             { return g_list_model_get_n_items(slots) == 18; });
    auto *scroll = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(grid));
    gtk_grid_view_scroll_to(grid, 17, GTK_LIST_SCROLL_NONE, nullptr);
    wait_for([&]
             { return gtk_adjustment_get_value(scroll) > 0 &&
                      grid_position_at(grid, width - 1, gtk_widget_get_height(widget) - 1) == 16; });
    g_assert_cmpuint(grid_position_at(grid, 0, gtk_widget_get_height(widget) - 1), ==, 19);
    gtk_window_set_child(desktop.window, nullptr);
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
    install_desktop_style(gdk_display_get_default());
    g_test_add_func("/file-actions/undo-copied-folders", undo_copied_folders);
    g_test_add_func("/file-actions/queued-trash", queued_trash);
    g_test_add_func("/file-actions/inline-replacement", inline_replacement);
    g_test_add_func("/file-actions/metadata-refresh", metadata_refresh);
    g_test_add_func("/file-actions/grid-density", grid_density);
    g_test_add_func("/file-actions/cut-feedback", cut_feedback);
    return g_test_run();
}
