#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>

ObjectPtr<GFile> make_copy_stage(GFile *desktop, GHashTable *staging, GApplication *app);
void remove_copy_stage(GFile *stage, GHashTable *staging, GApplication *app);
