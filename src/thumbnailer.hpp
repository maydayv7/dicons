#pragma once

#include "gobject_ptr.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

ObjectPtr<GdkPixbuf> run_thumbnailer(GFile *file, const char *mime, int size, GCancellable *cancel);
