#include "clipboard.hpp"
#include <string>

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/clipboard/formats", formats);
    g_test_add_func("/clipboard/malformed", malformed);
    return g_test_run();
}
