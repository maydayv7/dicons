#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>
#include <map>
#include <string>
#include <vector>

struct GridCell
{
    guint column;
    guint row;
};

using IconPositions = std::map<std::string, GridCell>;

IconPositions read_positions(GSettings *settings);
void relocate_position(GSettings *settings, GFile *from, GFile *to);

// Each monitor fits the shared positions to its viewport
class GridLayout
{
public:
    GridLayout(GListModel *items, GSettings *preferences);
    ~GridLayout();
    void resize(guint width, guint height);
    bool move(const std::vector<std::string> &uris, guint destination);

    ObjectPtr<GListStore> slots{g_list_store_new(G_TYPE_FILE_INFO)};
    ObjectPtr<GSettings> settings;
    guint columns = 1;
    guint rows = 1;

private:
    void positions_changed();
    void items_changed(guint first, guint removed, guint added);
    void arrange();
    ObjectPtr<GListModel> items_;
    IconPositions positions_;
};
