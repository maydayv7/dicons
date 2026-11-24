#include "desktop_item.hpp"

#include <gio/gdesktopappinfo.h>
#include <string_view>

struct DiconsItemUpdates
{
    GObject parent;
};
using DiconsItemUpdatesClass = GObjectClass;
G_DEFINE_TYPE(DiconsItemUpdates, item_updates, G_TYPE_OBJECT)

static guint item_updated;
static void item_updates_class_init(DiconsItemUpdatesClass *klass)
{
    item_updated = g_signal_new("item-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
                                nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_FILE_INFO);
}
static void item_updates_init(DiconsItemUpdates *) {}

ObjectPtr<DiconsItemUpdates> make_item_updates()
{
    return ObjectPtr<DiconsItemUpdates>{static_cast<DiconsItemUpdates *>(g_object_new(item_updates_get_type(), nullptr))};
}

void notify_item_updated(DiconsItemUpdates *updates, GFileInfo *info)
{
    g_signal_emit(updates, item_updated, 0, info);
}

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
