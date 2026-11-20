#include "desktop_sort.hpp"
#include "desktop_item.hpp"

#include <compare>
#include <string_view>
#include <utility>

namespace
{
    enum class Field
    {
        name,
        modified,
        size,
        type
    };

    struct Order
    {
        Field field = Field::name;
        bool descending = false;
        bool folders_first = true;
    };

    struct SortChoice
    {
        Field field;
        const char *id;
        const char *label;
    };

    constexpr SortChoice fields[] = {
        {Field::name, "name", "By File Name"},
        {Field::modified, "modified", "By Modification Time"},
        {Field::size, "size", "By File Size"},
        {Field::type, "type", "By File Type"},
    };
}

static auto modified_time(GFileInfo *info)
{
    return std::pair{g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                     g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC)};
}

static int compare_items(gconstpointer left, gconstpointer right, gpointer data)
{
    auto *a = G_FILE_INFO(left);
    auto *b = G_FILE_INFO(right);
    const auto &order = *static_cast<Order *>(data);
    if (order.folders_first)
    {
        bool a_folder = g_file_info_get_file_type(a) == G_FILE_TYPE_DIRECTORY;
        bool b_folder = g_file_info_get_file_type(b) == G_FILE_TYPE_DIRECTORY;
        if (a_folder != b_folder)
            return a_folder ? -1 : 1;
    }

    std::strong_ordering result = std::strong_ordering::equal;
    switch (order.field)
    {
    case Field::modified:
        result = modified_time(a) <=> modified_time(b);
        break;
    case Field::size:
        result = g_file_info_get_size(a) <=> g_file_info_get_size(b);
        break;
    case Field::type:
        result = g_strcmp0(g_file_info_get_content_type(a), g_file_info_get_content_type(b)) <=> 0;
        break;
    case Field::name:
        break;
    }
    if (result != 0)
    {
        int sign = result < 0 ? -1 : 1;
        return order.descending ? -sign : sign;
    }

    // Natural name order
    int by_name = g_strcmp0(filename_key(a), filename_key(b));
    if (!by_name)
        by_name = g_strcmp0(g_file_info_get_name(a), g_file_info_get_name(b));
    int sign = gtk_ordering_from_cmpfunc(by_name);
    return order.field == Field::name && order.descending ? -sign : sign;
}

static void sort_changed(GSettings *preferences, const char *, GtkCustomSorter *sorter)
{
    g_autofree char *field = g_settings_get_string(preferences, "sort-field");
    g_autofree char *direction = g_settings_get_string(preferences, "sort-direction");
    auto *order = new Order;
    for (const auto &choice : fields)
        if (std::string_view{field} == choice.id)
            order->field = choice.field;
    order->descending = std::string_view{direction} == "descending";
    order->folders_first = g_settings_get_boolean(preferences, "folders-first");
    gtk_custom_sorter_set_sort_func(sorter, compare_items, order, [](gpointer p)
                                    { delete static_cast<Order *>(p); });
}

static void sync_field(GSettings *preferences, const char *, GSimpleAction *field)
{
    g_autoptr(GVariant) value = g_settings_get_value(preferences, "sort-field");
    g_simple_action_set_state(field, value);
}

static void choose_field(GSimpleAction *action, GVariant *value, GSettings *preferences)
{
    g_autoptr(GVariant) current = g_action_get_state(G_ACTION(action));
    if (g_variant_equal(current, value))
        return;

    g_autoptr(GSettingsSchema) schema = nullptr;
    GSettingsBackend *backend = nullptr;
    g_object_get(preferences, "settings-schema", &schema, "backend", &backend, nullptr);
    ObjectPtr<GSettings> edit{g_settings_new_full(schema, backend, nullptr)};
    g_object_unref(backend);
    g_settings_delay(edit.get());
    if (!g_settings_set_value(edit.get(), "sort-field", value))
        return;
    if (g_settings_is_writable(edit.get(), "sort-direction"))
    {
        bool newest_first = std::string_view{g_variant_get_string(value, nullptr)} == "modified";
        g_settings_set_string(edit.get(), "sort-direction", newest_first ? "descending" : "ascending");
    }
    g_settings_apply(edit.get());
}

DesktopSort::DesktopSort(GSettings *preferences)
    : actions(g_simple_action_group_new()), sorter(gtk_custom_sorter_new(nullptr, nullptr, nullptr))
{
    g_autoptr(GVariant) initial = g_settings_get_value(preferences, "sort-field");
    ObjectPtr<GSimpleAction> field{g_simple_action_new_stateful("field", G_VARIANT_TYPE_STRING, initial)};
    g_settings_bind_writable(preferences, "sort-field", field.get(), "enabled", false);
    g_signal_connect_object(field.get(), "change-state", G_CALLBACK(choose_field), preferences, GConnectFlags{});
    g_signal_connect_object(preferences, "changed::sort-field", G_CALLBACK(sync_field), field.get(), GConnectFlags{});
    g_action_map_add_action(G_ACTION_MAP(actions.get()), G_ACTION(field.get()));
    for (const char *key : {"sort-direction", "folders-first"})
    {
        ObjectPtr<GAction> action{g_settings_create_action(preferences, key)};
        g_action_map_add_action(G_ACTION_MAP(actions.get()), action.get());
    }

    for (const char *signal : {"changed::sort-field", "changed::sort-direction", "changed::folders-first"})
        g_signal_connect_object(preferences, signal, G_CALLBACK(sort_changed), sorter.get(), GConnectFlags{});
    sort_changed(preferences, nullptr, sorter.get());
}

void append_sort_menu(GMenu *menu)
{
    g_autoptr(GMenu) sorting = g_menu_new();
    for (const auto &choice : fields)
    {
        g_autoptr(GMenuItem) item = g_menu_item_new(choice.label, nullptr);
        g_menu_item_set_action_and_target(item, "sort.field", "s", choice.id);
        g_menu_append_item(sorting, item);
    }
    g_autoptr(GMenu) direction = g_menu_new();
    g_menu_append(direction, "Ascending", "sort.sort-direction::ascending");
    g_menu_append(direction, "Descending", "sort.sort-direction::descending");
    g_menu_append_section(sorting, nullptr, G_MENU_MODEL(direction));
    g_autoptr(GMenu) grouping = g_menu_new();
    g_menu_append(grouping, "Folders First", "sort.folders-first");
    g_menu_append_section(sorting, nullptr, G_MENU_MODEL(grouping));
    g_menu_append_submenu(menu, "Sorting", G_MENU_MODEL(sorting));
}
