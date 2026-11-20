#include "preferences.hpp"

ObjectPtr<GSettings> desktop_preferences(GSettingsBackend *backend)
{
    auto *installed = g_settings_schema_source_get_default();
    g_autoptr(GSettingsSchema) schema = installed ? g_settings_schema_source_lookup(installed, desktop_app_id, true) : nullptr;
    if (!schema)
    {
        g_autofree char *executable = g_file_read_link("/proc/self/exe", nullptr);
        if (executable)
        {
            g_autofree char *directory = g_path_get_dirname(executable);
            g_autoptr(GSettingsSchemaSource) local = g_settings_schema_source_new_from_directory(directory, installed, false, nullptr);
            if (local)
                schema = g_settings_schema_source_lookup(local, desktop_app_id, false);
        }
    }
    if (!schema)
        g_error("Cannot find the GSettings schema %s. Check the dicons installation or GSETTINGS_SCHEMA_DIR.", desktop_app_id);
    return ObjectPtr<GSettings>{g_settings_new_full(schema, backend, nullptr)};
}
