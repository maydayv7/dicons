#include "clipboard.hpp"
#include "desktop_directory.hpp"
#include "grid_selection.hpp"
#include "notifications.hpp"
#include "transfer.hpp"

#include <array>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
    constexpr const char *uris = "text/uri-list";
    constexpr const char *gnome = "x-special/gnome-copied-files";
    constexpr const char *kde = "application/x-kde-cutselection";
    using CutIdentities = std::shared_ptr<const DragIdentities>;

    GdkContentProvider *bytes_provider(const char *mime, const std::string &text)
    {
        g_autoptr(GBytes) bytes = g_bytes_new(text.data(), text.size());
        return gdk_content_provider_new_for_bytes(mime, bytes);
    }

    void offer(GdkClipboard *clipboard, const std::vector<GFile *> &files, bool cut,
               CutIdentities identities)
    {
        g_autoptr(GdkContentProvider) content = file_clipboard_content(files, cut);
        if (identities)
            g_object_set_data_full(G_OBJECT(content), "cut-identities", new CutIdentities(std::move(identities)),
                                   [](gpointer p)
                                   { delete static_cast<CutIdentities *>(p); });
        gdk_clipboard_set_content(clipboard, content);
    }

    CutIdentities &foreign_cut(GdkClipboard *clipboard)
    {
        auto *ids = static_cast<CutIdentities *>(g_object_get_data(G_OBJECT(clipboard), "foreign-cut-identities"));
        if (!ids)
        {
            ids = new CutIdentities;
            g_object_set_data_full(G_OBJECT(clipboard), "foreign-cut-identities", ids, [](gpointer p)
                                   { delete static_cast<CutIdentities *>(p); });
            g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard *, CutIdentities *cached)
                                                              { cached->reset(); }),
                             ids);
        }
        return *ids;
    }

    struct Paste
    {
        ObjectPtr<GtkGridView> grid;
        ObjectPtr<GdkClipboard> clipboard;
        ObjectPtr<GFile> folder;
        DropIdentity target;
        CutIdentities identities;
        ObjectPtr<GCancellable> reading{g_cancellable_new()};
        bool changed{}, cut{};
        const char *mime{};

        void release_slot()
        {
            if (g_object_get_data(G_OBJECT(clipboard.get()), "dicons-paste") == this)
                g_object_set_data(G_OBJECT(clipboard.get()), "dicons-paste", nullptr);
        }

        ~Paste()
        {
            g_signal_handlers_disconnect_by_data(clipboard.get(), this);
            release_slot();
        }
    };

    void failed(Paste *request, const char *detail)
    {
        std::unique_ptr<Paste> paste{request};
        auto *root = gtk_widget_get_root(GTK_WIDGET(paste->grid.get()));
        if (!paste->changed && root)
            notify_error(G_APPLICATION(gtk_window_get_application(GTK_WINDOW(root))), "Could not paste files", detail);
    }

    void transfer(Paste *request, const std::vector<GFile *> &files)
    {
        std::shared_ptr<Paste> paste{request};
        if (paste->changed || files.empty())
            return;
        if (!paste->cut)
            paste->release_slot();
        transfer_pasted_files(paste->grid.get(), paste->folder.get(), paste->target, files, paste->cut, paste->identities,
                              [paste, count = files.size()](const std::vector<GFile *> &remaining, CutIdentities identities)
                              {
                                  if (!paste->cut || paste->changed)
                                      return;
                                  if (remaining.size() == count)
                                  {
                                      if (!paste->identities)
                                          foreign_cut(paste->clipboard.get()) = std::move(identities);
                                  }
                                  else if (remaining.empty())
                                      gdk_clipboard_set_content(paste->clipboard.get(), nullptr);
                                  else
                                      offer(paste->clipboard.get(), remaining, true, std::move(identities));
                              });
    }

    void file_list_ready(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *paste = static_cast<Paste *>(data);
        g_autoptr(GError) error = nullptr;
        auto *value = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(source), result, &error);
        if (!value)
            return failed(paste, error->message);
        std::vector<GFile *> files;
        auto *list = static_cast<GdkFileList *>(g_value_get_boxed(value));
        for (auto *link = gdk_file_list_get_files(list); link; link = link->next)
            files.push_back(G_FILE(link->data));
        transfer(paste, files);
    }

    void read_offer(Paste *paste, const char *mime);

    void read_list(Paste *paste)
    {
        if (gdk_content_formats_contain_mime_type(gdk_clipboard_get_formats(paste->clipboard.get()), uris))
            read_offer(paste, uris);
        else
            gdk_clipboard_read_value_async(paste->clipboard.get(), GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, paste->reading.get(), file_list_ready, paste);
    }

    void read_stream(GTask *task, gpointer source, gpointer, GCancellable *cancel)
    {
        std::string text;
        std::array<char, 16384> buffer;
        g_autoptr(GError) error = nullptr;
        for (;;)
        {
            auto count = g_input_stream_read(G_INPUT_STREAM(source), buffer.data(), buffer.size(), cancel, &error);
            if (count < 0)
            {
                g_task_return_error(task, g_steal_pointer(&error));
                return;
            }
            if (!count)
                break;
            text.append(buffer.data(), count);
            if (text.size() > 16 * 1024 * 1024)
            {
                g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The clipboard file list is too large.");
                return;
            }
        }
        if (text.find('\0') != text.npos)
        {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid clipboard data.");
            return;
        }
        g_task_return_pointer(task, g_strdup(text.c_str()), g_free);
    }

    void text_ready(GObject *, GAsyncResult *result, gpointer data)
    {
        auto *paste = static_cast<Paste *>(data);
        g_autoptr(GError) error = nullptr;
        g_autofree char *text = static_cast<char *>(g_task_propagate_pointer(G_TASK(result), &error));
        if (!text || paste->changed)
            return failed(paste, error ? error->message : "The clipboard changed.");
        if (paste->mime == kde)
        {
            std::string_view marker{g_strstrip(text)};
            if (marker != "1" && marker != "0")
                return failed(paste, "Invalid clipboard cut marker.");
            paste->cut = marker == "1";
            return read_list(paste);
        }
        auto entries = decode_file_clipboard(text, paste->mime == gnome, paste->cut, &error);
        if (error)
            return failed(paste, error->message);
        std::vector<GFile *> files;
        for (const auto &entry : entries)
            files.push_back(entry.get());
        transfer(paste, files);
    }

    void stream_ready(GObject *source, GAsyncResult *result, gpointer data)
    {
        auto *paste = static_cast<Paste *>(data);
        g_autoptr(GError) error = nullptr;
        ObjectPtr<GInputStream> stream{gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result, nullptr, &error)};
        if (!stream || paste->changed)
            return failed(paste, error ? error->message : "The clipboard changed.");
        ObjectPtr<GTask> task{g_task_new(stream.get(), paste->reading.get(), text_ready, paste)};
        g_task_run_in_thread(task.get(), read_stream);
    }

    void read_offer(Paste *paste, const char *mime)
    {
        paste->mime = mime;
        const char *types[] = {mime, nullptr};
        gdk_clipboard_read_async(paste->clipboard.get(), types, G_PRIORITY_DEFAULT, paste->reading.get(), stream_ready, paste);
    }
}

GdkContentProvider *file_clipboard_content(const std::vector<GFile *> &files, bool cut)
{
    std::string operation = cut ? "cut" : "copy";
    std::string list;
    for (auto *file : files)
    {
        g_autofree char *uri = g_file_get_uri(file);
        operation += "\n";
        operation += uri;
        list += uri;
        list += "\r\n";
    }
    GdkContentProvider *providers[] = {bytes_provider(gnome, operation), bytes_provider(uris, list), bytes_provider(kde, cut ? "1" : "0")};
    return gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
}

std::vector<ObjectPtr<GFile>> decode_file_clipboard(const char *text, bool has_operation, bool &cut, GError **error)
{
    if (has_operation)
    {
        const char *newline = strchr(text, '\n');
        std::string_view operation{text, newline ? size_t(newline - text) : 0};
        if (operation.ends_with('\r'))
            operation.remove_suffix(1);
        if (operation != "copy" && operation != "cut")
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Unknown clipboard operation.");
            return {};
        }
        cut = operation == "cut";
        text = newline + 1;
    }
    g_auto(GStrv) paths = g_uri_list_extract_uris(text);
    auto seen = make_file_set();
    std::vector<ObjectPtr<GFile>> files;
    for (char **path = paths; *path; ++path)
    {
        if (!g_uri_is_valid(*path, G_URI_FLAGS_NONE, nullptr))
        {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid file URI on the clipboard.");
            return {};
        }
        ObjectPtr<GFile> file{g_file_new_for_uri(*path)};
        if (g_hash_table_add(seen.get(), g_object_ref(file.get())))
            files.push_back(std::move(file));
    }
    if (files.empty())
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The clipboard file list is empty.");
    return files;
}

bool clipboard_has_files(GdkClipboard *clipboard)
{
    auto *formats = gdk_clipboard_get_formats(clipboard);
    return gdk_content_formats_contain_mime_type(formats, gnome) || gdk_content_formats_contain_mime_type(formats, uris) ||
           gdk_content_formats_contain_gtype(formats, GDK_TYPE_FILE_LIST);
}

void copy_selection(GtkGridView *grid, bool cut)
{
    auto files = selected_files(gtk_grid_view_get_model(grid));
    if (files.empty())
        return;
    std::shared_ptr<DragIdentities> identities;
    if (cut)
    {
        identities = std::make_shared<DragIdentities>();
        for (auto *file : files)
        {
            g_autofree char *uri = g_file_get_uri(file);
            identities->emplace(uri, file_identity(file));
        }
    }
    offer(gtk_widget_get_clipboard(GTK_WIDGET(grid)), files, cut, std::move(identities));
}

void paste_selection(GtkGridView *grid, bool into_folder)
{
    auto *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(grid));
    if (!clipboard_has_files(clipboard) || g_object_get_data(G_OBJECT(clipboard), "dicons-paste"))
        return;
    ObjectPtr<GFile> folder;
    DropIdentity target;
    if (into_folder)
    {
        auto *selection = gtk_grid_view_get_model(grid);
        g_autoptr(GtkBitset) selected = gtk_selection_model_get_selection(selection);
        if (gtk_bitset_get_size(selected) != 1)
            return;
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(G_LIST_MODEL(selection), gtk_bitset_get_minimum(selected)))};
        if (is_trash_icon(info.get()) || g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY)
            return;
        folder = retain(item_file(info.get()));
        target = DropIdentity(info.get());
    }
    auto *paste = new Paste{retain(grid), retain(clipboard), std::move(folder), std::move(target)};
    if (auto *content = gdk_clipboard_get_content(clipboard))
        if (auto *ids = static_cast<CutIdentities *>(g_object_get_data(G_OBJECT(content), "cut-identities")))
            paste->identities = *ids;
    if (!paste->identities)
        paste->identities = foreign_cut(clipboard);
    g_object_set_data(G_OBJECT(clipboard), "dicons-paste", paste);
    g_signal_connect(clipboard, "changed", G_CALLBACK(+[](GdkClipboard *, Paste *p)
                                                      { p->changed = true; g_cancellable_cancel(p->reading.get()); p->release_slot(); }),
                     paste);
    auto *formats = gdk_clipboard_get_formats(clipboard);
    if (gdk_content_formats_contain_mime_type(formats, gnome))
        read_offer(paste, gnome);
    else if (gdk_content_formats_contain_mime_type(formats, kde))
        read_offer(paste, kde);
    else
        read_list(paste);
}
