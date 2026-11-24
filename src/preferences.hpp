#pragma once

#include "gobject_ptr.hpp"
#include <gio/gio.h>

inline constexpr char desktop_app_id[] = "dev.maydayv7.dicons";

ObjectPtr<GSettings> desktop_preferences(GSettingsBackend *backend = nullptr);
bool applying_keyfile_declarations(GSettings *settings);
