#include "desktop_item.hpp"

#include <gio/gdesktopappinfo.h>
#include <string_view>

bool has_launcher_name(GFileInfo *info)
{
    return std::string_view{g_file_info_get_name(info)}.ends_with(".desktop");
}

bool is_desktop_launcher(GFileInfo *info)
{
    return g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR && has_launcher_name(info);
}

void read_launcher_presentation(GFileInfo *info, GFile *file)
{
    if (!is_desktop_launcher(info))
        return;
    g_autoptr(GDesktopAppInfo) launcher = g_desktop_app_info_new_from_filename(g_file_peek_path(file));
    if (launcher)
    {
        auto *app = G_APP_INFO(launcher);
        g_file_info_set_attribute_string(info, "dicons::label", g_app_info_get_display_name(app));
        if (auto *icon = g_app_info_get_icon(app))
            g_file_info_set_icon(info, icon);
    }
}

const char *item_label(GFileInfo *info)
{
    const char *label = g_file_info_get_attribute_string(info, "dicons::label");
    return label ? label : g_file_info_get_display_name(info);
}

void cache_filename_key(GFileInfo *info)
{
    g_autofree char *folded = g_utf8_casefold(item_label(info), -1);
    g_autofree char *key = g_utf8_collate_key_for_filename(folded, -1);
    g_file_info_set_attribute_byte_string(info, "dicons::name-key", key);
}

const char *filename_key(GFileInfo *info)
{
    return g_file_info_get_attribute_byte_string(info, "dicons::name-key");
}
