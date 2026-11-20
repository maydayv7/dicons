#include "launch.hpp"
#include "desktop_directory.hpp"
#include "desktop_item.hpp"
#include "notifications.hpp"

#include <gio/gdesktopappinfo.h>

static void default_opened(GObject *, GAsyncResult *result, gpointer data)
{
    g_autoptr(GApplication) app = G_APPLICATION(data);
    g_autoptr(GError) error = nullptr;
    if (!g_app_info_launch_default_for_uri_finish(result, &error))
        notify_error(app, "Could not open item", error->message);
}

static void trash_opened(GObject *source, GAsyncResult *result, gpointer data)
{
    ObjectPtr<GApplication> app{G_APPLICATION(data)};
    g_autoptr(GError) error = nullptr;
    if (!g_app_info_launch_uris_finish(G_APP_INFO(source), result, &error))
        notify_error(app.get(), "Could not open Trash", error->message);
}

void open_item(GtkGridView *grid, guint position, GtkApplication *app)
{
    auto *model = G_LIST_MODEL(gtk_grid_view_get_model(grid));
    g_autoptr(GFileInfo) item = static_cast<GFileInfo *>(g_list_model_get_item(model, position));
    if (!item || !item_file(item))
        return;
    GFile *file = item_file(item);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(grid));
    g_autoptr(GdkAppLaunchContext) context = gdk_display_get_app_launch_context(display);
    g_autoptr(GError) error = nullptr;

    if (is_trash_icon(item))
    {
        // File managers that advertise inode/directory without registering x-scheme-handler/trash
        ObjectPtr<GAppInfo> handler{g_app_info_get_default_for_uri_scheme("trash")};
        if (!handler)
            handler.reset(g_app_info_get_default_for_type("inode/directory", true));
        if (handler)
        {
            g_autofree char *uri = g_file_get_uri(file);
            GList uris{uri};
            g_app_info_launch_uris_async(handler.get(), &uris, G_APP_LAUNCH_CONTEXT(context), nullptr,
                                         trash_opened, g_object_ref(app));
            return;
        }
    }
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

static void terminal_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    ObjectPtr<GApplication> app{G_APPLICATION(data)};
    g_autoptr(GError) error = nullptr;
    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error))
        notify_error(app.get(), "Could not open terminal", error->message);
}

void open_terminal(GtkApplication *app, GFile *folder)
{
    g_autoptr(GError) error = nullptr;
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_set_cwd(launcher, g_file_peek_path(folder));
    g_autofree char *argument = g_strdup_printf("--dir=%s", g_file_peek_path(folder));
    g_autoptr(GSubprocess) process = g_subprocess_launcher_spawn(launcher, &error, "xdg-terminal-exec", argument, nullptr);
    if (!process)
    {
        notify_file_error(G_APPLICATION(app), "Could not open terminal", folder, error);
        return;
    }
    g_subprocess_wait_check_async(process, nullptr, terminal_finished, g_object_ref(app));
}
