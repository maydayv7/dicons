#include "undo.hpp"
#include "grid_selection.hpp"
#include "grid_layout.hpp"
#include "notifications.hpp"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <set>
#include <variant>
#include <utility>
#include <unistd.h>

namespace
{
    using Variant = std::shared_ptr<GVariant>;
    Variant value(GVariant *owned) { return Variant(owned, g_variant_unref); }

    enum class Removal
    {
        trash,
        empty_folder,
        copied_folder
    };
    struct FileChange
    {
        ObjectPtr<GFile> current, original;
        DropIdentity item, parent, previous;
        Removal removal = Removal::trash;
    };
    struct Restored
    {
        ObjectPtr<GFile> original, actual;
        DropIdentity previous, current;
    };
    using Change = std::variant<FileChange, TrashedFile>;

    struct History
    {
        std::vector<UndoToken> past;
        std::deque<UndoToken> requested;
        bool running{};
        uint64_t sequence{};
        ObjectPtr<GSettings> preferences;
        ObjectPtr<GFile> desktop;
    };

    History &history(GApplication *app)
    {
        auto *state = static_cast<History *>(g_object_get_data(G_OBJECT(app), "desktop-undo"));
        if (!state)
        {
            state = new History;
            g_object_set_data_full(G_OBJECT(app), "desktop-undo", state, [](gpointer p)
                                   { delete static_cast<History *>(p); });
        }
        return *state;
    }
}

struct UndoStep
{
    std::vector<Change> files;
    std::function<void()> view;
    ObjectPtr<GtkGridView> grid;
    uint64_t sequence{};
    Variant prior_positions;
    IconPositions positions;
    std::vector<Restored> restored;
    std::vector<std::shared_ptr<const Restored>> rebind;
    std::vector<std::string> errors;
    bool ready{};
    size_t remaining{};
    bool confirmed{};
};

static void run_next(GApplication *app);
static void rebind_file(Change &file, const Restored &mapping);

static void trim_history(History &state)
{
    constexpr size_t limit = 32;
    if (state.past.size() <= limit)
        return;
    size_t completed = std::ranges::count_if(state.past, &UndoStep::ready);
    while (completed > limit)
    {
        state.past.erase(std::ranges::find_if(state.past, &UndoStep::ready));
        --completed;
    }
}

void attach_undo_preferences(GApplication *app, GSettings *settings, GFile *desktop)
{
    auto &state = history(app);
    state.preferences = retain(settings);
    state.desktop = retain(desktop);
}

UndoToken begin_undo_step(GApplication *app)
{
    auto step = std::make_shared<UndoStep>();
    auto &state = history(app);
    step->sequence = ++state.sequence;
    if (state.preferences)
        step->prior_positions = value(g_settings_get_value(state.preferences.get(), "icon-positions"));
    state.past.push_back(step);
    return step;
}

void remember_new_file(const UndoToken &step, GFile *result, FileOrigin origin)
{
    ObjectPtr<GFileInfo> info{g_file_query_info(result, drop_identity_attributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr)};
    FileChange change{.current = retain(result), .item = DropIdentity(info.get())};
    if (info && g_file_info_get_file_type(info.get()) == G_FILE_TYPE_DIRECTORY)
        change.removal = origin == FileOrigin::copied ? Removal::copied_folder : Removal::empty_folder;
    step->files.emplace_back(std::move(change));
}

void remember_move(const UndoToken &step, GFile *result, GFile *original, DropIdentity item, DropIdentity previous)
{
    if (g_file_equal(result, original))
        return;
    if (previous.id.empty())
        previous = item;
    ObjectPtr<GFile> parent{g_file_get_parent(original)};
    step->files.emplace_back(FileChange{retain(result), retain(original), std::move(item), file_identity(parent.get(), G_FILE_QUERY_INFO_NONE), std::move(previous)});
}

void remember_trash(const UndoToken &step, TrashedFile entry)
{
    step->files.emplace_back(std::move(entry));
}

void finish_undo_step(GApplication *app, const UndoToken &step)
{
    // Only moves and Trash can return an item to a saved cell
    auto snapshot = std::move(step->prior_positions);
    if (snapshot && g_variant_n_children(snapshot.get()))
    {
        std::set<std::string, std::less<>> returning;
        for (const auto &change : step->files)
        {
            auto *file = std::visit([](const auto &item)
                                    { return item.original.get(); }, change);
            if (file)
            {
                g_autofree char *uri = g_file_get_uri(file);
                returning.emplace(uri);
            }
        }
        GVariantIter iter;
        g_variant_iter_init(&iter, snapshot.get());
        const char *uri;
        GridCell cell;
        while (step->positions.size() < returning.size() && g_variant_iter_next(&iter, "{&s(uu)}", &uri, &cell.column, &cell.row))
            if (returning.contains(uri))
                step->positions.emplace(uri, cell);
    }
    for (const auto &mapping : step->rebind)
        for (auto &file : step->files)
            rebind_file(file, *mapping);
    step->rebind.clear();
    step->ready = true;
    auto &state = history(app);
    if (step->files.empty() && !step->view)
        std::erase(state.past, step);
    else
        trim_history(state);
    run_next(app);
}

static void undo_files(GTask *task, gpointer, gpointer data, GCancellable *)
{
    auto &step = *static_cast<UndoStep *>(data);
    while (step.remaining)
    {
        const size_t i = step.remaining - 1;
        const bool confirmed = std::exchange(step.confirmed, false);
        GError *raw = nullptr;
        ObjectPtr<GFile> restored;
        bool done = false;
        GFile *file;
        if (auto *trash = std::get_if<TrashedFile>(&step.files[i]))
        {
            file = trash->original.get();
            restored = restore_from_trash(*trash, &raw);
            done = restored || (g_error_matches(raw, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) && !trash_entry_available(*trash));
        }
        else
        {
            auto &change = std::get<FileChange>(step.files[i]);
            file = change.current.get();
            if (!change.item.matches(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, &raw))
                done = g_error_matches(raw, G_IO_ERROR, G_IO_ERROR_FAILED) || g_error_matches(raw, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
            else if (!change.original && change.removal != Removal::trash && !confirmed)
            {
                done = rmdir(g_file_peek_path(file)) == 0;
                if (!done)
                {
                    const int code = errno;
                    if (code == ENOTEMPTY || code == EEXIST)
                    {
                        if (change.removal == Removal::copied_folder)
                        {
                            g_task_return_boolean(task, false);
                            return;
                        }
                        g_set_error_literal(&raw, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY, "The new folder contains items. Empty it before undoing its creation.");
                    }
                    else
                        g_set_error_literal(&raw, G_IO_ERROR, g_io_error_from_errno(code), g_strerror(code));
                }
            }
            else if (!change.original)
                done = g_file_trash(file, nullptr, &raw);
            else
            {
                ObjectPtr<GFile> parent{g_file_get_parent(change.original.get())};
                if (change.parent.matches(parent.get(), G_FILE_QUERY_INFO_NONE, &raw) &&
                    rename_no_replace(file, change.original.get(), &raw, &change.item))
                {
                    done = true;
                    restored = retain(change.original.get());
                }
            }
        }
        g_autoptr(GError) error = raw;
        if (error)
        {
            g_autofree char *name = g_file_get_parse_name(file);
            step.errors.emplace_back(std::string(name) + "\n" + error->message);
        }
        if (restored)
        {
            auto now = file_identity(restored.get());
            auto original = std::visit([](const auto &item)
                                       { return retain(item.original.get()); }, step.files[i]);
            auto *change = std::get_if<FileChange>(&step.files[i]);
            step.restored.push_back({std::move(original), std::move(restored), change ? change->previous : now, now});
        }
        if (done)
            step.files.erase(step.files.begin() + i);
        --step.remaining;
    }
    g_task_return_boolean(task, true);
}

static void continue_undo(GApplication *app);

static void confirm_folder_undo(GApplication *app, UndoStep &step)
{
    auto *parent = step.grid ? GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(step.grid.get()))) : nullptr;
    if (!parent || !gtk_widget_get_mapped(GTK_WIDGET(parent)))
    {
        --step.remaining;
        continue_undo(app);
        return;
    }
    const auto &folder = std::get<FileChange>(step.files[step.remaining - 1]).current;
    g_autofree char *path = g_file_get_parse_name(folder.get());
    ObjectPtr<GtkAlertDialog> dialog{gtk_alert_dialog_new("Move this copied folder and its contents to Trash?")};
    g_autofree char *detail = g_strdup_printf("%s\n\nThis includes items added since the folder was copied.", path);
    gtk_alert_dialog_set_detail(dialog.get(), detail);
    const char *buttons[] = {"Keep Folder", "Move to Trash", nullptr};
    gtk_alert_dialog_set_buttons(dialog.get(), buttons);
    gtk_alert_dialog_set_cancel_button(dialog.get(), 0);
    gtk_alert_dialog_set_default_button(dialog.get(), 0);
    ObjectPtr<GCancellable> cancel{g_cancellable_new()};
    g_signal_connect_object(parent, "unmap", G_CALLBACK(g_cancellable_cancel), cancel.get(), G_CONNECT_SWAPPED);
    gtk_alert_dialog_choose(dialog.get(), parent, cancel.get(), [](GObject *source, GAsyncResult *result, gpointer data)
                            {
                                ObjectPtr<GApplication> app{G_APPLICATION(data)};
                                auto &step = *history(app.get()).requested.front();
                                step.confirmed = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr) == 1;
                                if (!step.confirmed)
                                    --step.remaining;
                                continue_undo(app.get()); }, g_object_ref(app));
}

static void undo_finished(GObject *source, GAsyncResult *result, gpointer)
{
    auto *app = G_APPLICATION(source);
    auto &state = history(app);
    auto step = state.requested.front();
    if (!g_task_propagate_boolean(G_TASK(result), nullptr))
    {
        confirm_folder_undo(app, *step);
        return;
    }
    state.requested.pop_front();
    for (const auto &error : step->errors)
        notify_error(app, "Could not fully undo operation", error.c_str());
    step->errors.clear();
    IconPositions positions;
    bool place = false;
    bool select_first = true;
    for (auto &item : step->restored)
    {
        if (state.desktop && g_file_has_parent(item.actual.get(), state.desktop.get()))
        {
            g_autofree char *from = g_file_get_uri(item.original.get());
            if (auto cell = step->positions.find(from); cell != step->positions.end())
            {
                if (!place)
                    positions = read_positions(state.preferences.get());
                g_autofree char *to = g_file_get_uri(item.actual.get());
                positions[to] = cell->second;
                place = true;
            }
            if (step->grid && gtk_widget_get_root(GTK_WIDGET(step->grid.get())))
            {
                if (select_first)
                    gtk_selection_model_unselect_all(gtk_grid_view_get_model(step->grid.get()));
                select_first = false;
                select_when_listed(step->grid.get(), item.actual.get());
            }
        }
        if (item.previous != item.current || !g_file_equal(item.original.get(), item.actual.get()))
        {
            auto remap = std::make_shared<Restored>(std::move(item));
            auto rebind = [&](const UndoToken &older)
            {
                if (older->sequence >= step->sequence)
                    return;
                if (older->ready)
                {
                    for (auto &file : older->files)
                        rebind_file(file, *remap);
                }
                else
                    older->rebind.push_back(remap);
            };
            std::ranges::for_each(state.past, rebind);
            std::ranges::for_each(state.requested, rebind);
        }
    }
    if (place)
        write_positions(state.preferences.get(), positions);
    step->restored.clear();
    step->grid.reset();
    if (!step->files.empty())
    {
        state.past.insert(std::ranges::lower_bound(state.past, step->sequence, {}, &UndoStep::sequence), step);
        trim_history(state);
    }
    state.running = false;
    run_next(app);
}

static void rebind_child(ObjectPtr<GFile> &path, const Restored &mapping)
{
    if (!path)
        return;
    g_autofree char *relative = g_file_get_relative_path(mapping.original.get(), path.get());
    if (relative && *relative)
        path.reset(g_file_resolve_relative_path(mapping.actual.get(), relative));
}

static void rebind_file(Change &file, const Restored &mapping)
{
    std::visit([&](auto &item)
               { rebind_child(item.original, mapping); }, file);
    if (auto *change = std::get_if<FileChange>(&file))
    {
        if (change->item == mapping.previous && g_file_equal(change->current.get(), mapping.original.get()))
        {
            change->current = retain(mapping.actual.get());
            change->item = mapping.current;
        }
        else
            rebind_child(change->current, mapping);
    }
}

static void continue_undo(GApplication *app)
{
    ObjectPtr<GTask> task{g_task_new(app, nullptr, undo_finished, nullptr)};
    g_task_set_task_data(task.get(), history(app).requested.front().get(), nullptr);
    g_task_run_in_thread(task.get(), undo_files);
}

static void run_next(GApplication *app)
{
    auto &state = history(app);
    while (!state.running && !state.requested.empty() && state.requested.front()->ready)
    {
        auto step = state.requested.front();
        if (!step->files.empty())
        {
            state.running = true;
            step->remaining = step->files.size();
            continue_undo(app);
            return;
        }
        if (step->view)
            step->view();
        state.requested.pop_front();
    }
}

void undo_latest(GApplication *app, GtkGridView *grid)
{
    auto &state = history(app);
    if (state.past.empty())
        return;
    auto step = state.past.back();
    state.past.pop_back();
    if (grid)
        step->grid = retain(grid);
    state.requested.push_back(std::move(step));
    run_next(app);
}

void change_view(GApplication *app, GSettings *settings, GAction *hidden, const std::function<void()> &change)
{
    // Restore positions after the sort keys
    using Setting = std::pair<const char *, Variant>;
    std::vector<Setting> before;
    for (auto key : {"sort-field", "sort-direction", "folders-first", "show-thumbnails", "show-trash", "icon-size", "icon-positions"})
        before.emplace_back(key, value(g_settings_get_value(settings, key)));
    auto old_hidden = hidden ? value(g_action_get_state(hidden)) : Variant{};
    change();
    std::erase_if(before, [&](const Setting &setting)
                  {
        auto current = value(g_settings_get_value(settings, setting.first));
        return g_variant_equal(current.get(), setting.second.get()); });
    if (hidden)
    {
        g_autoptr(GVariant) current = g_action_get_state(hidden);
        if (g_variant_equal(current, old_hidden.get()))
            old_hidden.reset();
    }
    if (before.empty() && !old_hidden)
        return;
    auto step = begin_undo_step(app);
    auto settings_ref = std::shared_ptr<GSettings>(g_object_ref(settings), g_object_unref);
    auto hidden_ref = old_hidden ? std::shared_ptr<GAction>(g_object_ref(hidden), g_object_unref) : nullptr;
    step->view = [settings_ref, hidden_ref, before, old_hidden]
    {
        for (const auto &[key, previous] : before)
            g_settings_set_value(settings_ref.get(), key, previous.get());
        if (old_hidden)
            g_action_change_state(hidden_ref.get(), old_hidden.get());
    };
    finish_undo_step(app, step);
}

ObjectPtr<GAction> undoable_view_action(GAction *action, GApplication *app, GSettings *settings, GAction *hidden)
{
    g_autoptr(GVariant) initial = g_action_get_state(action);
    ObjectPtr<GAction> proxy{G_ACTION(g_simple_action_new_stateful(g_action_get_name(action), g_action_get_parameter_type(action), initial))};
    for (auto property : {"enabled", "state"})
        g_object_bind_property(action, property, proxy.get(), property, G_BINDING_SYNC_CREATE);
    struct Binding
    {
        ObjectPtr<GAction> action;
        GApplication *app;
        GSettings *settings;
        GAction *hidden;
    };
    auto *binding = new Binding{retain(action), app, settings, hidden};
    g_signal_connect_data(proxy.get(), "change-state", G_CALLBACK(+[](GSimpleAction *, GVariant *next, Binding *binding)
                                                                  { change_view(binding->app, binding->settings, binding->hidden, [&]
                                                                                { g_action_change_state(binding->action.get(), next); }); }),
                          binding, [](gpointer p, GClosure *)
                          { delete static_cast<Binding *>(p); }, GConnectFlags{});
    return proxy;
}
