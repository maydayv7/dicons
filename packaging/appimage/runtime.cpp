#include <gtk/gtk.h>
#include <clocale>

static void restore_variable(const char *name, const char *value)
{
    if (value)
        g_setenv(name, value, true);
    else
        g_unsetenv(name);
}

// Called before GTK creates threads
// Cache private resource paths in this process and restore the session environment before launching any apps
void init_appimage()
{
    g_autofree char *exe = g_file_read_link("/proc/self/exe", nullptr);
    g_return_if_fail(exe);
    g_autofree char *bin = g_path_get_dirname(exe);
    g_autofree char *share = g_build_filename(bin, "..", "share", nullptr);
    if (!std::setlocale(LC_ALL, ""))
    {
        g_autofree char *old_path = g_strdup(g_getenv("LOCPATH"));
        g_autofree char *locale = g_build_filename(bin, "..", "lib", "locale", nullptr);
        g_setenv("LOCPATH", locale, true);
        if (std::setlocale(LC_ALL, "C.UTF-8"))
            gtk_disable_setlocale();
        restore_variable("LOCPATH", old_path);
    }
    g_autofree char *previous = g_strdup(g_getenv("XDG_DATA_DIRS"));
    g_autofree char *dirs = g_strconcat(previous && *previous ? previous : "/usr/local/share:/usr/share", ":", share, nullptr);
    g_setenv("XDG_DATA_DIRS", dirs, true);
    g_get_system_data_dirs();
    g_content_type_set_mime_dirs(nullptr);
    restore_variable("XDG_DATA_DIRS", previous);

    g_autofree char *modules = g_build_filename(bin, "..", "lib", "gio", nullptr);
    g_vfs_get_default(); // Discover host GIO modules before registering the fallback
    auto *point = g_io_extension_point_lookup("gsettings-backend");
    if (!point || !g_io_extension_point_get_extension_by_name(point, "dconf"))
        g_io_modules_scan_all_in_directory(modules);
    g_autofree char *cache = g_build_filename(share, "loaders.cache", nullptr);
    g_autofree char *old_cache = g_strdup(g_getenv("GDK_PIXBUF_MODULE_FILE"));
    g_setenv("GDK_PIXBUF_MODULE_FILE", cache, true);
    g_slist_free(gdk_pixbuf_get_formats());
    restore_variable("GDK_PIXBUF_MODULE_FILE", old_cache);
}
