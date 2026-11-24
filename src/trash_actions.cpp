#include "undo.hpp"
#include "trash_actions.hpp"
#include "desktop_directory.hpp"
#include "grid_selection.hpp"
#include "notifications.hpp"
#include "trash_file.hpp"

#include <deque>
#include <string>

namespace
{
    struct Request
    {
        UndoToken undo;
        ObjectPtr<GtkGridView> grid;
        std::vector<std::pair<ObjectPtr<GFile>, DropIdentity>> files;
        std::function<void(bool)> finished;
        std::vector<TrashedFile> entries;
        struct Error
        {
            const char *title;
            std::string detail;
        };
        std::vector<Error> errors;
    };

    struct History
    {
        std::deque<Request> queue;
        FileSet pending = make_file_set();
    };

    History &history(GApplication *app)
    {
        auto *state = static_cast<History *>(g_object_get_data(G_OBJECT(app), "trash-history"));
        if (!state)
        {
            state = new History;
            g_object_set_data_full(G_OBJECT(app), "trash-history", state, [](gpointer p)
                                   { delete static_cast<History *>(p); });
        }
        return *state;
    }

    void record_error(Request &request, const char *title, GFile *file, const char *message)
    {
        g_autofree char *name = g_file_get_parse_name(file);
        request.errors.push_back({title, std::string{name} + "\n" + message});
    }

    void work(GTask *task, gpointer, gpointer data, GCancellable *)
    {
        auto &request = *static_cast<Request *>(data);
        for (const auto &[file, identity] : request.files)
        {
            TrashedFile entry;
            g_autoptr(GError) error = nullptr;
            if (!move_to_trash(file.get(), entry, &error, &identity))
                record_error(request, "Could not move item to Trash", file.get(), error->message);
            else
            {
                if (entry.stored)
                    request.entries.push_back(std::move(entry));
                else
                    record_error(request, "Undo unavailable", file.get(), "Moved to Trash, but its location could not be recorded.");
            }
        }
        g_task_return_boolean(task, true);
    }

    void start(GApplication *app);

    void completed(GObject *source, GAsyncResult *, gpointer)
    {
        auto *app = G_APPLICATION(source);
        auto &state = history(app);
        auto &request = state.queue.front();
        for (const auto &[file, identity] : request.files)
            g_hash_table_remove(state.pending.get(), file.get());
        for (auto &entry : request.entries)
            remember_trash(request.undo, std::move(entry));
        finish_undo_step(app, request.undo);
        for (const auto &error : request.errors)
            notify_error(app, error.title, error.detail.c_str());
        if (request.finished)
            request.finished(request.errors.empty());
        state.queue.pop_front();
        start(app);
    }

    void start(GApplication *app)
    {
        auto &state = history(app);
        if (state.queue.empty())
            return;
        auto &request = state.queue.front();
        ObjectPtr<GTask> task{g_task_new(app, nullptr, completed, nullptr)};
        g_task_set_task_data(task.get(), &request, nullptr);
        g_task_run_in_thread(task.get(), work);
    }

    GApplication *application(GtkGridView *grid)
    {
        auto *window = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(grid)));
        return G_APPLICATION(gtk_window_get_application(window));
    }
}

void trash_files(GtkGridView *origin, const std::vector<GFile *> &files, std::function<void(bool)> finished,
                 const DragIdentities *identities)
{
    auto *app = application(origin);
    auto &state = history(app);
    Request request{.grid = retain(origin), .finished = std::move(finished)};
    for (auto *file : files)
    {
        if (!g_hash_table_add(state.pending.get(), g_object_ref(file)))
            continue;
        DropIdentity identity;
        if (identities)
        {
            g_autofree char *uri = g_file_get_uri(file);
            if (auto it = identities->find(uri); it != identities->end())
                identity = it->second;
        }
        else
            identity = file_identity(file);
        request.files.emplace_back(retain(file), std::move(identity));
    }
    if (request.files.empty())
    {
        if (request.finished)
            request.finished(false);
        return;
    }
    request.undo = begin_undo_step(app);
    state.queue.push_back(std::move(request));
    if (state.queue.size() == 1)
        start(app);
}
