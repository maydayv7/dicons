#include "create_item.hpp"
#include "copy.hpp"
#include "copy_stage.hpp"
#include "desktop_directory.hpp"
#include "grid_selection.hpp"
#include "notifications.hpp"

#include <format>

namespace
{
    struct Creation
    {
        ObjectPtr<GtkGridView> grid;
        ObjectPtr<GApplication> app;
        ObjectPtr<GFile> desktop;
        NewItem kind;
        ObjectPtr<GFile> source;
        ObjectPtr<GFile> stage;
        FileSet staging;
    };
}

static ObjectPtr<GFile> create_unique(const Creation &request, GError **error)
{
    if (request.source)
    {
        auto item = desktop_copy_item(request.source.get(), request.desktop.get(), request.stage.get(), false, error);
        if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            return item;
        g_clear_error(error);
        return desktop_copy_item(request.source.get(), request.desktop.get(), request.stage.get(), true, error);
    }

    const char *base = request.kind == NewItem::folder ? "New Folder" : "New File";
    for (guint n = 0;; ++n)
    {
        auto name = n ? std::format("{} ({})", base, n) : std::string(base);
        ObjectPtr<GFile> file{g_file_get_child(request.desktop.get(), name.c_str())};
        if (request.kind == NewItem::folder)
        {
            if (g_file_make_directory(file.get(), nullptr, error))
                return file;
        }
        else
        {
            g_autoptr(GFileOutputStream) stream = g_file_create(file.get(), G_FILE_CREATE_NONE, nullptr, error);
            if (stream && g_output_stream_close(G_OUTPUT_STREAM(stream), nullptr, error))
                return file;
        }
        if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            return nullptr;
        g_clear_error(error);
    }
}

static void create_worker(GTask *task, gpointer, gpointer data, GCancellable *)
{
    g_autoptr(GError) error = nullptr;
    auto item = create_unique(*static_cast<Creation *>(data), &error);
    if (item)
        g_task_return_pointer(task, item.release(), g_object_unref);
    else
        g_task_return_error(task, g_steal_pointer(&error));
}

static void created(GObject *, GAsyncResult *result, gpointer)
{
    auto *request = static_cast<Creation *>(g_task_get_task_data(G_TASK(result)));
    g_autoptr(GError) error = nullptr;
    ObjectPtr<GFile> item{G_FILE(g_task_propagate_pointer(G_TASK(result), &error))};
    if (request->stage)
        remove_copy_stage(request->stage.get(), request->staging.get(), request->app.get());
    if (error)
        notify_file_error(request->app.get(), "Could not create item",
                          request->source ? request->source.get() : request->desktop.get(), error);
    else if (gtk_widget_get_root(GTK_WIDGET(request->grid.get())))
        select_when_listed(request->grid.get(), item.get(), ArrivalAction::rename);
}

void create_item(GtkGridView *grid, const DesktopDirectory &directory, NewItem kind, GFile *source)
{
    auto *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
    auto *app = G_APPLICATION(gtk_window_get_application(window));
    auto request = std::make_unique<Creation>();
    request->grid = retain(grid);
    request->app = retain(app);
    request->desktop = retain(directory.folder.get());
    request->kind = kind;
    if (source)
    {
        request->source = retain(source);
        request->staging.reset(g_hash_table_ref(directory.staging.get()));
        request->stage = make_copy_stage(directory.folder.get(), directory.staging.get(), app);
        if (!request->stage)
            return;
    }
    ObjectPtr<GTask> task{g_task_new(nullptr, nullptr, created, nullptr)};
    g_task_set_task_data(task.get(), request.release(), [](gpointer p)
                         { delete static_cast<Creation *>(p); });
    g_task_run_in_thread(task.get(), create_worker);
}
