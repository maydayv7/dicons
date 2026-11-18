#include "notifications.hpp"

void notify_error(GApplication *app, const char *title, const char *detail)
{
    g_printerr("%s: %s\n", title, detail);
    GDBusConnection *bus = g_application_get_dbus_connection(app);
    if (!bus)
        return;

    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("dicons"));
    g_autofree char *body = g_markup_escape_text(detail, -1);
    auto *args = g_variant_new("(susss@asa{sv}i)", "Desktop", 0u, "folder", title, body,
                               g_variant_new_strv(nullptr, 0), &hints, -1);
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                           "org.freedesktop.Notifications", "Notify", args, G_VARIANT_TYPE("(u)"),
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}
