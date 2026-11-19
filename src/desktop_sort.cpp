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

static void sort_changed(GActionGroup *actions, const char *name, GVariant *, GtkCustomSorter *sorter)
{
    g_autoptr(GVariant) field = g_action_group_get_action_state(actions, "field");
    g_autoptr(GVariant) direction = g_action_group_get_action_state(actions, "direction");
    std::string_view field_id = g_variant_get_string(field, nullptr);
    bool descending = std::string_view{g_variant_get_string(direction, nullptr)} == "descending";
    if (std::string_view{name} == "field" && descending != (field_id == "modified"))
    {
        const char *initial = field_id == "modified" ? "descending" : "ascending";
        g_action_group_change_action_state(actions, "direction", g_variant_new_string(initial));
        return;
    }

    g_autoptr(GVariant) folders = g_action_group_get_action_state(actions, "folders-first");
    auto *order = new Order;
    for (const auto &choice : fields)
        if (field_id == choice.id)
            order->field = choice.field;
    order->descending = descending;
    order->folders_first = g_variant_get_boolean(folders);
    gtk_custom_sorter_set_sort_func(sorter, compare_items, order, [](gpointer p)
                                    { delete static_cast<Order *>(p); });
}

DesktopSort::DesktopSort()
    : actions(g_simple_action_group_new()),
      sorter(gtk_custom_sorter_new(compare_items, new Order, [](gpointer p)
                                   { delete static_cast<Order *>(p); }))
{
    const GActionEntry entries[] = {
        {"field", nullptr, "s", "'name'"},
        {"direction", nullptr, "s", "'ascending'"},
        {"folders-first", nullptr, nullptr, "true"},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(actions.get()), entries, G_N_ELEMENTS(entries), nullptr);
    g_signal_connect_object(actions.get(), "action-state-changed", G_CALLBACK(sort_changed), sorter.get(), GConnectFlags{});
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
    g_menu_append(direction, "Ascending", "sort.direction::ascending");
    g_menu_append(direction, "Descending", "sort.direction::descending");
    g_menu_append_section(sorting, nullptr, G_MENU_MODEL(direction));
    g_autoptr(GMenu) grouping = g_menu_new();
    g_menu_append(grouping, "Folders First", "sort.folders-first");
    g_menu_append_section(sorting, nullptr, G_MENU_MODEL(grouping));
    g_menu_append_submenu(menu, "Sorting", G_MENU_MODEL(sorting));
}
