#pragma once

#include <gtk/gtk.h>

struct DesktopDirectory;
void install_desktop_style(GdkDisplay *display);
void sync_windows(GtkApplication *app, GListModel *monitors, const DesktopDirectory &directory);
