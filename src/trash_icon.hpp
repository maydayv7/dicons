#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>

bool is_trash_icon(GFileInfo *info);

class TrashIcon
{
public:
    TrashIcon();
    ~TrashIcon();
    ObjectPtr<GSimpleAction> visible{g_simple_action_new_stateful("show-trash", nullptr, g_variant_new_boolean(true))};
    ObjectPtr<GListStore> items{g_list_store_new(G_TYPE_FILE_INFO)};

private:
    ObjectPtr<GFileInfo> info_{g_file_info_new()};
    ObjectPtr<GFileMonitor> monitor_;
    ObjectPtr<GCancellable> cancel_{g_cancellable_new()};
    bool reading_{};
    bool dirty_{};
    ObjectPtr<GFile> location_;
    bool local_{};
    static void visibility_changed(GSimpleAction *action, GParamSpec *, TrashIcon *self);
    static void state_read(GObject *, GAsyncResult *result, gpointer data);
    void watch_local();
    void refresh();
};
