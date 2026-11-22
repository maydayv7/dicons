#pragma once

#include "file_rename.hpp"
#include "trash_file.hpp"
#include <gtk/gtk.h>
#include <functional>

struct UndoStep;
using UndoToken = std::shared_ptr<UndoStep>;
void attach_undo_preferences(GApplication *app, GSettings *settings);
UndoToken begin_undo_step(GApplication *app);
void remember_file(const UndoToken &step, GFile *result, GFile *original = nullptr);
void remember_trash(const UndoToken &step, TrashedFile entry);
void finish_undo_step(GApplication *app, const UndoToken &step);
void undo_latest(GApplication *app, GtkGridView *grid = nullptr);

void change_view(GApplication *app, GSettings *settings, GAction *hidden, const std::function<void()> &change);
ObjectPtr<GAction> undoable_view_action(GAction *action, GApplication *app, GSettings *settings, GAction *hidden);
