#include "launch.hpp"
#include "desktop_directory.hpp"
#include "desktop_item.hpp"
#include "notifications.hpp"

#include <unistd.h>

static void start_app_session(gpointer)
{
    setsid();
}

gboolean launch_desktop_app(GDesktopAppInfo *handler, GList *uris, GAppLaunchContext *context, GError **error)
{
    return g_desktop_app_info_launch_uris_as_manager(handler, uris, context, G_SPAWN_SEARCH_PATH,
                                                     start_app_session, nullptr, nullptr, nullptr, error);
}

static void default_opened(GObject *source, GAsyncResult *result, gpointer data)
{
    g_autoptr(GApplication) app = G_APPLICATION(data);
    g_autoptr(GError) error = nullptr;
    bool launched = G_IS_APP_INFO(source) ? g_app_info_launch_uris_finish(G_APP_INFO(source), result, &error)
                                          : g_app_info_launch_default_for_uri_finish(result, &error);
    if (!launched)
        notify_error(app, "Could not open item", error->message);
}

static void open_uri(GFile *file, GAppInfo *handler, GAppLaunchContext *context, GApplication *app, const char *title)
{
    g_autofree char *uri = g_file_get_uri(file);
    GList uris{uri};
    if (G_IS_DESKTOP_APP_INFO(handler))
    {
        g_autoptr(GError) error = nullptr;
        if (!launch_desktop_app(G_DESKTOP_APP_INFO(handler), &uris, context, &error))
            notify_error(app, title, error->message);
        return;
    }
    if (handler)
    {
        g_app_info_launch_uris_async(handler, &uris, context, nullptr, default_opened, g_object_ref(app));
        return;
    }
    g_app_info_launch_default_for_uri_async(uri, context, nullptr, default_opened, g_object_ref(app));
}

struct OpenRequest
{
    ObjectPtr<GAppLaunchContext> context;
    ObjectPtr<GApplication> app;
};

static void handler_ready(GObject *source, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<OpenRequest> request{static_cast<OpenRequest *>(data)};
    auto *file = G_FILE(source);
    ObjectPtr<GAppInfo> handler{g_file_query_default_handler_finish(file, result, nullptr)};
    open_uri(file, handler.get(), request->context.get(), request->app.get(), "Could not open item");
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
            open_uri(file, handler.get(), G_APP_LAUNCH_CONTEXT(context), G_APPLICATION(app), "Could not open Trash");
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
                    launch_desktop_app(launcher, nullptr, G_APP_LAUNCH_CONTEXT(context), &error);
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

    auto *request = new OpenRequest{retain(G_APP_LAUNCH_CONTEXT(context)), retain(G_APPLICATION(app))};
    g_file_query_default_handler_async(file, G_PRIORITY_DEFAULT, nullptr, handler_ready, request);
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
    g_subprocess_launcher_set_child_setup(launcher, start_app_session, nullptr, nullptr);
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
