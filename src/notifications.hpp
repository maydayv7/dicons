#pragma once

#include <gio/gio.h>

void notify_error(GApplication *app, const char *title, const char *detail);
void notify_file_error(GApplication *app, const char *title, GFile *file, const GError *error);
