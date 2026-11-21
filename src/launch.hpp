#pragma once

#include <gtk/gtk.h>
#include <gio/gdesktopappinfo.h>

gboolean launch_desktop_app(GDesktopAppInfo *handler, GList *uris, GAppLaunchContext *context, GError **error);
void open_item(GtkGridView *grid, guint position, GtkApplication *app);
void open_terminal(GtkApplication *app, GFile *folder);
