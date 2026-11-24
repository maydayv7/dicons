#include "grid_layout.hpp"
#include "desktop_directory.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>

IconPositions read_positions(GSettings *settings)
{
    IconPositions result;
    g_autoptr(GVariant) value = g_settings_get_value(settings, "icon-positions");
    GVariantIter iter;
    g_variant_iter_init(&iter, value);
    const char *uri;
    GridCell cell;
    while (g_variant_iter_next(&iter, "{&s(uu)}", &uri, &cell.column, &cell.row))
        result.emplace(uri, cell);
    return result;
}

bool write_positions(GSettings *settings, const IconPositions &positions)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{s(uu)}"));
    for (const auto &[uri, cell] : positions)
        g_variant_builder_add(&builder, "{s(uu)}", uri.c_str(), cell.column, cell.row);
    return g_settings_set_value(settings, "icon-positions", g_variant_builder_end(&builder));
}

void relocate_position(GSettings *settings, GFile *from, GFile *to)
{
    auto positions = read_positions(settings);
    g_autofree char *uri = g_file_get_uri(from);
    auto entry = positions.extract(uri);
    if (entry.empty())
        return;
    if (to)
    {
        g_autofree char *destination = g_file_get_uri(to);
        positions[destination] = entry.mapped();
    }
    write_positions(settings, positions);
}

GridLayout::GridLayout(GListModel *items, GSettings *preferences)
    : settings(retain(preferences)), items_(retain(items))
{
    positions_changed();
    g_signal_connect(items_.get(), "items-changed", G_CALLBACK(+[](GListModel *, guint first, guint removed, guint added, GridLayout *self)
                                                               { self->items_changed(first, removed, added); }),
                     this);
    g_signal_connect(settings.get(), "changed::icon-positions", G_CALLBACK(+[](GSettings *, const char *, GridLayout *self)
                                                                           { self->positions_changed(); }),
                     this);
}

GridLayout::~GridLayout()
{
    g_signal_handlers_disconnect_by_data(items_.get(), this);
    g_signal_handlers_disconnect_by_data(settings.get(), this);
}

void GridLayout::items_changed(guint first, guint removed, guint added)
{
    if (!positions_.empty())
    {
        arrange();
        return;
    }
    g_autoptr(GPtrArray) batch = g_ptr_array_new_with_free_func(g_object_unref);
    for (guint i = first; i < first + added; ++i)
        g_ptr_array_add(batch, g_list_model_get_item(items_.get(), i));
    g_list_store_splice(slots.get(), first, removed, batch->pdata, added);
}

void GridLayout::positions_changed()
{
    positions_ = read_positions(settings.get());
    arrange();
}

void GridLayout::resize(guint width, guint height)
{
    if (columns == width && rows == height)
        return;
    columns = width;
    rows = height;
    if (!positions_.empty())
        arrange();
}

static void replace_slots(GListStore *slots, std::vector<gpointer> &next)
{
    auto *model = G_LIST_MODEL(slots);
    const guint old_count = g_list_model_get_n_items(model);
    std::vector<ObjectPtr<GFileInfo>> old;
    for (guint i = 0; i < old_count; ++i)
        old.emplace_back(G_FILE_INFO(g_list_model_get_item(model, i)));
    std::vector<ObjectPtr<GFileInfo>> blanks;
    for (size_t i = 0; i < next.size(); ++i)
    {
        if (!next[i])
        {
            if (i < old.size() && !item_file(old[i].get()))
                next[i] = old[i].get();
            else
            {
                blanks.emplace_back(g_file_info_new());
                next[i] = blanks.back().get();
            }
        }
    }
    // Keep unaffected tiles bound
    size_t first = 0, end = next.size(), old_end = old.size();
    while (first < end && first < old_end && next[first] == old[first].get())
        ++first;
    while (end > first && old_end > first && next[end - 1] == old[old_end - 1].get())
    {
        --end;
        --old_end;
    }
    if (end != first || old_end != first)
        g_list_store_splice(slots, first, old_end - first, end > first ? next.data() + first : nullptr, end - first);
}

std::vector<gpointer> GridLayout::arranged_slots(GFile *arrival) const
{
    const auto count = g_list_model_get_n_items(items_.get());
    std::vector<GFileInfo *> entries;
    entries.reserve(count);
    // The directory monitor may already have listed the new file before its transfer completes.
    for (guint i = 0; i < count; ++i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(items_.get(), i))};
        if (!arrival || !g_file_equal(item_file(info.get()), arrival))
            entries.push_back(info.get());
    }
    const guint total = entries.size() + (arrival != nullptr);
    const guint limit = std::max(rows, (total + columns - 1) / columns);
    std::vector<gpointer> next(entries.size());
    std::vector<GFileInfo *> unplaced;
    for (auto *info : entries)
    {
        g_autofree char *uri = g_file_get_uri(item_file(info));
        auto found = positions_.find(uri);
        if (found == positions_.end() || found->second.column >= columns || found->second.row >= limit)
        {
            unplaced.push_back(info);
            continue;
        }
        auto [x, y] = found->second;
        guint index = y * columns + x;
        if (index >= next.size())
            next.resize(index + 1);
        if (next[index])
            unplaced.push_back(info);
        else
            next[index] = info;
    }
    size_t cursor = 0;
    for (auto *info : unplaced)
    {
        while (cursor < next.size() && next[cursor])
            ++cursor;
        if (cursor == next.size())
            next.push_back(info);
        else
            next[cursor] = info;
        ++cursor;
    }

    return next;
}

void GridLayout::arrange()
{
    auto next = arranged_slots();
    replace_slots(slots.get(), next);
}

void GridLayout::place_drop(GFile *arrival, GridCell anchor)
{
    g_autofree char *uri = g_file_get_uri(arrival);
    if (!g_settings_is_writable(settings.get(), "icon-positions"))
        return;
    auto existing = arranged_slots(arrival);
    std::set<guint64> reserved;
    for (const auto &[name, cell] : positions_)
        if (name != uri && cell.column < columns)
            reserved.insert(guint64(cell.row) * columns + cell.column);
    // Search the current grid first
    guint count = std::ranges::count_if(existing, [](gpointer item)
                                        { return item != nullptr; });
    guint limit = std::max(rows, count / columns + 1);
    anchor.column = std::min(anchor.column, columns - 1);
    anchor.row = std::min(anchor.row, limit - 1);
    GridCell nearest{};
    guint64 best = std::numeric_limits<guint64>::max();
    for (guint y = 0; y < limit || best == std::numeric_limits<guint64>::max(); ++y)
        for (guint x = 0; x < columns; ++x)
        {
            guint64 slot = guint64(y) * columns + x;
            if ((slot < existing.size() && existing[slot]) || reserved.contains(slot))
                continue;
            guint64 distance = std::abs(gint64(x) - anchor.column) + std::abs(gint64(y) - anchor.row);
            if (distance < best)
            {
                nearest = {x, y};
                best = distance;
            }
        }
    auto saved = positions_;
    saved[uri] = nearest;
    write_positions(settings.get(), saved);
}

bool GridLayout::move(const std::vector<std::string> &uris, guint destination)
{
    auto *model = G_LIST_MODEL(slots.get());
    const guint count = g_list_model_get_n_items(model);
    const guint limit = std::max(rows, count ? (count - 1) / columns + 1 : 0);
    if (uris.empty() || destination / columns >= limit || !g_settings_is_writable(settings.get(), "icon-positions"))
        return false;
    auto saved = read_positions(settings.get());
    IconPositions current;
    for (guint i = 0; i < count; ++i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(model, i))};
        if (auto *file = item_file(info.get()))
        {
            g_autofree char *uri = g_file_get_uri(file);
            current.emplace(uri, GridCell{i % columns, i / columns});
        }
    }
    auto anchor = current.find(uris.front());
    if (anchor == current.end())
        return false;
    int dx = int(destination % columns) - int(anchor->second.column);
    int dy = int(destination / columns) - int(anchor->second.row);
    std::set<std::string> moving(uris.begin(), uris.end());
    std::set<guint> occupied;
    for (const auto &[uri, cell] : current)
    {
        if (!moving.contains(uri))
        {
            occupied.insert(cell.row * columns + cell.column);
            saved.try_emplace(uri, cell);
        }
    }
    for (const auto &uri : uris)
    {
        auto found = current.find(uri);
        if (found == current.end())
            return false;
        int x = int(found->second.column) + dx;
        int y = int(found->second.row) + dy;
        if (x < 0 || x >= int(columns) || y < 0 || guint(y) >= limit ||
            occupied.contains(guint(y) * columns + x))
            return false;
        saved[uri] = {guint(x), guint(y)};
    }
    return (!dx && !dy) || write_positions(settings.get(), saved);
}
