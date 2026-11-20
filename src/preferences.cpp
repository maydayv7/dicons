#include "preferences.hpp"

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

static bool dconf_available(GSettingsBackend *backend)
{
    auto *point = g_io_extension_point_lookup(G_SETTINGS_BACKEND_EXTENSION_POINT_NAME);
    auto *dconf = g_io_extension_point_get_extension_by_name(point, "dconf");
    if (!dconf || !G_TYPE_CHECK_INSTANCE_TYPE(backend, g_io_extension_get_type(dconf)))
        return false;

    ObjectPtr<GDBusConnection> bus{g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)};
    if (!bus)
        return false;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus.get(), "ca.desrt.dconf", "/",
                                                            "org.freedesktop.DBus.Peer", "Ping", nullptr, G_VARIANT_TYPE_UNIT,
                                                            G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    return reply != nullptr;
}

static ObjectPtr<GSettings> user_preferences(GSettingsSchema *schema)
{
    ObjectPtr<GSettingsBackend> backend{g_settings_backend_get_default()};
    ObjectPtr<GSettings> system{g_settings_new_full(schema, backend.get(), nullptr)};
    const char *requested = g_getenv("GSETTINGS_BACKEND");
    if (requested && *requested && !g_str_equal(requested, "dconf") && !g_str_equal(requested, "keyfile"))
        return system;
    if (dconf_available(backend.get()))
        return system;

    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "dicons", "settings.ini", nullptr);
    bool first_use = !g_file_test(path, G_FILE_TEST_EXISTS);
    ObjectPtr<GSettingsBackend> file{g_keyfile_settings_backend_new(path, g_settings_schema_get_path(schema), "desktop")};
    if (first_use)
    {
        ObjectPtr<GSettings> initial{g_settings_new_full(schema, file.get(), nullptr)};
        g_settings_delay(initial.get());
        g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
        for (char **key = keys; *key; ++key)
        {
            g_autoptr(GVariant) value = g_settings_get_user_value(system.get(), *key);
            if (value)
                g_settings_set_value(initial.get(), *key, value);
        }
        g_settings_apply(initial.get());
    }
    return ObjectPtr<GSettings>{g_settings_new_full(schema, file.get(), nullptr)};
}

ObjectPtr<GSettings> desktop_preferences(GSettingsBackend *backend)
{
    auto *installed = g_settings_schema_source_get_default();
    g_autoptr(GSettingsSchema) schema = nullptr;
    g_autofree char *executable = g_file_read_link("/proc/self/exe", nullptr);
    if (executable)
    {
        g_autofree char *directory = g_path_get_dirname(executable);
        g_autoptr(GSettingsSchemaSource) local = g_settings_schema_source_new_from_directory(directory, installed, false, nullptr);
        if (local)
            schema = g_settings_schema_source_lookup(local, desktop_app_id, false);
    }
    if (!schema && installed)
        schema = g_settings_schema_source_lookup(installed, desktop_app_id, true);
    if (!schema)
        g_error("Cannot find the GSettings schema %s. Check the dicons installation or GSETTINGS_SCHEMA_DIR.", desktop_app_id);
    return backend ? ObjectPtr<GSettings>{g_settings_new_full(schema, backend, nullptr)} : user_preferences(schema);
}
