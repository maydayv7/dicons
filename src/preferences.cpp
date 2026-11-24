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

bool applying_keyfile_declarations(GSettings *settings)
{
    return g_object_get_data(G_OBJECT(settings), "dicons-applying-declarations") != nullptr;
}

static ObjectPtr<GFile> declaration_file()
{
    return ObjectPtr<GFile>{g_file_new_build_filename(g_get_user_config_dir(), "dicons", "keyfile-settings.gv", nullptr)};
}

static GVariant *read_declarations(GFile *file, GError **error)
{
    g_autofree char *contents = nullptr;
    gsize length = 0;
    if (g_file_load_contents(file, nullptr, &contents, &length, nullptr, error))
        return g_variant_parse(G_VARIANT_TYPE_VARDICT, contents, contents + length, nullptr, error);
    if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        return nullptr;
    g_clear_error(error);
    return g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
}

static void apply_declarations(GFile *file, GSettings *preferences)
{
    g_autoptr(GError) error = nullptr;
    g_autoptr(GVariant) next = read_declarations(file, &error);
    if (!next)
    {
        g_warning("Cannot read %s: %s", g_file_peek_path(file), error->message);
        return;
    }

    g_autoptr(GSettingsSchema) schema = nullptr;
    g_autoptr(GSettingsBackend) backend = nullptr;
    g_object_get(preferences, "settings-schema", &schema, "backend", &backend, nullptr);
    ObjectPtr<GSettings> edit{g_settings_new_full(schema, backend, nullptr)};
    g_settings_delay(edit.get());
    ObjectPtr<GSettings> state{g_settings_get_child(edit.get(), "keyfile")};
    g_autoptr(GVariant) previous = g_settings_get_value(state.get(), "declarations");
    if (g_variant_equal(previous, next))
        return;

    // Store the new snapshot in the same transaction as the edits
    g_auto(GStrv) keys = g_settings_schema_list_keys(schema);
    gsize known = 0;
    for (char **key = keys; *key; ++key)
    {
        g_autoptr(GVariant) before = g_variant_lookup_value(previous, *key, nullptr);
        g_autoptr(GVariant) after = g_variant_lookup_value(next, *key, nullptr);
        if (after)
        {
            ++known;
            if (before && g_variant_equal(before, after))
                continue;
            g_autoptr(GSettingsSchemaKey) spec = g_settings_schema_get_key(schema, *key);
            if (!g_variant_is_of_type(after, g_settings_schema_key_get_value_type(spec)) ||
                !g_settings_schema_key_range_check(spec, after) || !g_settings_set_value(edit.get(), *key, after))
            {
                g_warning("Cannot apply '%s' from %s", *key, g_file_peek_path(file));
                return;
            }
        }
        else if (before)
        {
            if (!g_settings_is_writable(edit.get(), *key))
            {
                g_warning("Cannot reset '%s' after its declaration was removed", *key);
                return;
            }
            g_settings_reset(edit.get(), *key);
        }
    }
    if (known != g_variant_n_children(next))
    {
        g_warning("Unknown or duplicate setting in %s", g_file_peek_path(file));
        return;
    }
    if (!g_settings_set_value(state.get(), "declarations", next))
    {
        g_warning("Cannot record the declarations from %s", g_file_peek_path(file));
        return;
    }
    g_object_set_data(G_OBJECT(preferences), "dicons-applying-declarations", GINT_TO_POINTER(1));
    g_settings_apply(edit.get());
    g_object_set_data(G_OBJECT(preferences), "dicons-applying-declarations", nullptr);
}

static ObjectPtr<GSettings> user_preferences(GSettingsSchema *schema)
{
    ObjectPtr<GSettingsBackend> backend{g_settings_backend_get_default()};
    ObjectPtr<GSettings> system{g_settings_new_full(schema, backend.get(), nullptr)};
    const char *requested = g_getenv("GSETTINGS_BACKEND");
    if (requested && *requested && !g_str_equal(requested, "dconf") && !g_str_equal(requested, "keyfile"))
        return system;
    auto declarations = declaration_file();
    if (!g_file_query_exists(declarations.get(), nullptr) && dconf_available(backend.get()))
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
    ObjectPtr<GSettings> preferences{g_settings_new_full(schema, file.get(), nullptr)};
    g_autoptr(GError) error = nullptr;
    ObjectPtr<GFileMonitor> monitor{g_file_monitor_file(declarations.get(), G_FILE_MONITOR_NONE, nullptr, &error)};
    if (monitor)
    {
        // A rename event may name the temporary symlink
        g_signal_connect_object(monitor.get(), "changed", G_CALLBACK(+[](GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, GSettings *settings)
                                                                     { apply_declarations(declaration_file().get(), settings); }),
                                preferences.get(), GConnectFlags{});
        g_object_set_data_full(G_OBJECT(preferences.get()), "dicons-declaration-monitor", monitor.release(), g_object_unref);
    }
    else
        g_warning("Cannot watch %s: %s", g_file_peek_path(declarations.get()), error->message);
    apply_declarations(declarations.get(), preferences.get());
    return preferences;
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
