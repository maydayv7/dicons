#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>

struct DesktopSort
{
    explicit DesktopSort(GSettings *preferences);

    ObjectPtr<GSimpleActionGroup> actions;
    ObjectPtr<GtkCustomSorter> sorter;
};

void append_sort_menu(GMenu *menu);
