#include "clipboard.hpp"
#include "desktop_directory.hpp"
#include "transfer.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static std::string read_offer(GdkContentProvider *content, const char *mime)
{
    g_autoptr(GOutputStream) stream = g_memory_output_stream_new_resizable();
    g_autoptr(GAsyncResult) result = nullptr;
    gdk_content_provider_write_mime_type_async(content, mime, stream, G_PRIORITY_DEFAULT, nullptr, [](GObject *, GAsyncResult *r, gpointer data)
                                               { *static_cast<GAsyncResult **>(data) = G_ASYNC_RESULT(g_object_ref(r)); }, &result);
    while (!result)
        g_main_context_iteration(nullptr, TRUE);
    g_assert_true(gdk_content_provider_write_mime_type_finish(content, result, nullptr));
    auto *memory = G_MEMORY_OUTPUT_STREAM(stream);
    return {static_cast<const char *>(g_memory_output_stream_get_data(memory)), g_memory_output_stream_get_data_size(memory)};
}

static void formats()
{
    ObjectPtr<GFile> link{g_file_new_for_path("/example/folder link")};
    ObjectPtr<GFile> file{g_file_new_for_path("/example/100% #雪")};
    for (bool cut : {false, true})
    {
        g_autoptr(GdkContentProvider) content = file_clipboard_content({link.get(), file.get()}, cut);
        g_assert_true(read_offer(content, "application/x-kde-cutselection") == (cut ? "1" : "0"));
        for (bool gnome : {false, true})
        {
            auto data = read_offer(content, gnome ? "x-special/gnome-copied-files" : "text/uri-list");
            bool moving = gnome ? !cut : cut;
            g_autoptr(GError) error = nullptr;
            auto files = decode_file_clipboard(data.c_str(), gnome, moving, &error);
            g_assert_no_error(error);
            g_assert_true(moving == cut);
            g_assert_cmpuint(files.size(), ==, 2);
            g_assert_true(g_file_equal(link.get(), files[0].get()));
            g_assert_true(g_file_equal(file.get(), files[1].get()));
        }
    }
}

static void malformed()
{
    bool cut = false;
    for (auto text : {"", "move\nfile:///a", "cut", "cut\n# comment\n", "copy\nfile:///good\nrelative"})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_true(decode_file_clipboard(text, true, cut, &error).empty());
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    }
    g_autoptr(GError) error = nullptr;
    auto files = decode_file_clipboard("cut\r\nfile:///a\r\n# comment\r\nfile:///a\r\n", true, cut, &error);
    g_assert_no_error(error);
    g_assert_true(cut);
    g_assert_cmpuint(files.size(), ==, 1);
}

static void wait_for(auto predicate)
{
    const auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!predicate())
    {
        g_assert_true(g_get_monotonic_time() < deadline);
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(1000);
    }
}

static GtkGridView *grid_for(GtkApplication *app, const fs::path &directory)
{
    auto *window = GTK_WINDOW(gtk_application_window_new(app));
    auto *selection = gtk_multi_selection_new(G_LIST_MODEL(g_list_store_new(G_TYPE_FILE_INFO)));
    auto *grid = GTK_GRID_VIEW(gtk_grid_view_new(GTK_SELECTION_MODEL(selection), nullptr));
    gtk_window_set_child(window, GTK_WIDGET(grid));
    ObjectPtr<GFile> folder{g_file_new_for_path(directory.c_str())};
    transfer_attach(window, grid, folder.get(), make_file_set().get());
    return grid;
}

static void foreign_cut_retries()
{
    if (!g_getenv("DICONS_TEST_PRIVATE_CLIPBOARD"))
    {
        g_test_skip("Set DICONS_TEST_PRIVATE_CLIPBOARD on an isolated GTK display");
        return;
    }
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    g_assert_true(g_application_register(G_APPLICATION(app.get()), nullptr, nullptr));
    const auto root = fs::path(g_get_user_data_dir());
    const auto source = root / "source", target = root / "target";
    fs::create_directories(source);
    fs::create_directories(target);
    auto *first = grid_for(app.get(), source);
    auto *second = grid_for(app.get(), target);
    auto *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(first));
    auto paste = [&](GtkGridView *grid)
    {
        paste_selection(grid, false);
        wait_for([&]
                 { return !g_object_get_data(G_OBJECT(clipboard), "dicons-paste"); });
    };

    auto path = target / "remaining";
    ObjectPtr<GFile> remaining{g_file_new_for_path(path.c_str())};
    ObjectPtr<GFile> moving{g_file_new_for_path((source / "moving").c_str())};
    for (bool partial : {false, true})
    {
        std::ofstream(path) << "original";
        std::vector<GFile *> entries{remaining.get()};
        if (partial)
        {
            std::ofstream(source / "moving") << "moving";
            entries.push_back(moving.get());
        }
        ObjectPtr<GdkContentProvider> offer{file_clipboard_content(entries, true)};
        g_assert_true(gdk_clipboard_set_content(clipboard, offer.get()));
        if (partial)
            paste(second);
        else
        {
            std::ofstream(source / "remaining") << "existing";
            paste_selection(first, false);
            ObjectPtr<GtkWindow> dialog;
            auto *windows = gtk_window_get_toplevels();
            auto *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(first)));
            wait_for([&]
                     {
                for (guint i = 0; i < g_list_model_get_n_items(windows); ++i)
                {
                    dialog.reset(GTK_WINDOW(g_list_model_get_item(windows, i)));
                    if (gtk_window_get_transient_for(dialog.get()) == parent)
                        return true;
                }
                return false; });
            gtk_window_close(dialog.get());
            wait_for([&]
                     { return !g_object_get_data(G_OBJECT(clipboard), "dicons-paste"); });
            g_assert_true(fs::exists(path));
            fs::remove(source / "remaining");
        }
        auto *retry = gdk_clipboard_get_content(clipboard);
        g_assert_nonnull(retry);
        if (partial)
        {
            g_assert_true(fs::exists(target / "moving"));
            g_assert_false(fs::exists(source / "moving"));
            g_assert_true(retry != offer.get());
        }
        else
            g_assert_true(retry == offer.get());
        g_autofree char *uri = g_file_get_uri(remaining.get());
        g_assert_true(read_offer(retry, "x-special/gnome-copied-files") == std::string("cut\n") + uri);

        fs::rename(path, target / "old");
        fs::create_symlink("old", path);
        paste(first);
        g_assert_true(fs::is_symlink(path));
        g_assert_false(fs::is_symlink(source / "remaining"));
        g_assert_true(gdk_clipboard_get_content(clipboard) == retry);

        if (partial)
        {
            ObjectPtr<GdkContentProvider> fresh{file_clipboard_content({remaining.get()}, true)};
            g_assert_true(gdk_clipboard_set_content(clipboard, fresh.get()));
        }
        else
        {
            fs::remove(path);
            fs::rename(target / "old", path);
        }
        paste(first);
        g_assert_null(gdk_clipboard_get_content(clipboard));
        g_assert_true(partial ? fs::is_symlink(source / "remaining") : fs::is_regular_file(source / "remaining"));
        fs::remove(source / "remaining");
    }

    const auto cancelled = source / "cancelled";
    std::ofstream(cancelled) << "keep here";
    ObjectPtr<GFile> entry{g_file_new_for_path(cancelled.c_str())};
    ObjectPtr<GdkContentProvider> old_offer{file_clipboard_content({entry.get()}, true)};
    g_assert_true(gdk_clipboard_set_content(clipboard, old_offer.get()));
    paste_selection(second, false);
    std::ofstream(source / "new") << "new offer";
    ObjectPtr<GFile> newer{g_file_new_for_path((source / "new").c_str())};
    ObjectPtr<GdkContentProvider> new_offer{file_clipboard_content({newer.get()}, true)};
    g_assert_true(gdk_clipboard_set_content(clipboard, new_offer.get()));
    paste(second);
    g_assert_true(fs::exists(target / "new"));
    g_assert_true(fs::exists(cancelled));
    g_assert_false(fs::exists(target / "cancelled"));
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(first))));
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(second))));
}

int main(int argc, char **argv)
{
    if (g_getenv("DICONS_TEST_PRIVATE_CLIPBOARD"))
        gtk_init();
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/clipboard/formats", formats);
    g_test_add_func("/clipboard/malformed", malformed);
    g_test_add_func("/clipboard/foreign-cut-retries", foreign_cut_retries);
    return g_test_run();
}
