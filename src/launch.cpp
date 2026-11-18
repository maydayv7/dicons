#include "launch.hpp"
#include "desktop_directory.hpp"
#include "notifications.hpp"

#include <gio/gdesktopappinfo.h>
#include <string_view>

bool has_launcher_name(GFileInfo *info)
{
    return std::string_view{g_file_info_get_name(info)}.ends_with(".desktop");
}

static void default_opened(GObject *, GAsyncResult *result, gpointer data)
{
    g_autoptr(GApplication) app = G_APPLICATION(data);
    g_autoptr(GError) error = nullptr;
    if (!g_app_info_launch_default_for_uri_finish(result, &error))
        notify_error(app, "Could not open item", error->message);
}

void open_item(GtkGridView *grid, guint position, GtkApplication *app)
{
    auto *model = G_LIST_MODEL(gtk_grid_view_get_model(grid));
    g_autoptr(GFileInfo) item = static_cast<GFileInfo *>(g_list_model_get_item(model, position));
    if (!item)
        return;
    GFile *file = item_file(item);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(grid));
    g_autoptr(GdkAppLaunchContext) context = gdk_display_get_app_launch_context(display);
    g_autoptr(GError) error = nullptr;

    if (has_launcher_name(item))
    {
        g_autoptr(GFileInfo) current = g_file_query_info(file, "standard::type,access::can-execute",
                                                         G_FILE_QUERY_INFO_NONE, nullptr, &error);
        if (current && g_file_info_get_file_type(current) == G_FILE_TYPE_REGULAR)
        {
            if (!g_file_info_get_attribute_boolean(current, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
            {
                g_set_error(&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "%s needs execute permission.", g_file_info_get_display_name(item));
            }
            else
            {
                g_autoptr(GDesktopAppInfo) launcher =
                    g_desktop_app_info_new_from_filename(g_file_peek_path(file));
                if (!launcher)
                    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Invalid launcher: %s", g_file_info_get_display_name(item));
                else
                    g_app_info_launch(G_APP_INFO(launcher), nullptr, G_APP_LAUNCH_CONTEXT(context), &error);
            }
            if (!error)
                return;
        }
        if (error)
        {
            notify_error(G_APPLICATION(app), "Could not open launcher", error->message);
            return;
        }
    }

    g_autofree char *uri = g_file_get_uri(file);
    g_app_info_launch_default_for_uri_async(uri, G_APP_LAUNCH_CONTEXT(context), nullptr,
                                            default_opened, g_object_ref(app));
}
