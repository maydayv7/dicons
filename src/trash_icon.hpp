#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>

bool is_trash_icon(GFileInfo *info);
struct DiconsItemUpdates;

class TrashIcon
{
public:
    TrashIcon(GSettings *preferences, DiconsItemUpdates *updates);
    ~TrashIcon();
    ObjectPtr<GAction> visible;
    ObjectPtr<GListStore> items{g_list_store_new(G_TYPE_FILE_INFO)};

private:
    DiconsItemUpdates *updates_;
    ObjectPtr<GFileInfo> info_{g_file_info_new()};
    ObjectPtr<GFileMonitor> monitor_;
    ObjectPtr<GCancellable> cancel_{g_cancellable_new()};
    bool reading_{};
    bool dirty_{};
    ObjectPtr<GFile> location_;
    bool local_{};
    static void visibility_changed(GAction *action, GParamSpec *, TrashIcon *self);
    static void state_read(GObject *, GAsyncResult *result, gpointer data);
    void watch_local();
    void refresh();
};
