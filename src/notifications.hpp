#pragma once

#include <gio/gio.h>

void notify_error(GApplication *app, const char *title, const char *detail);
