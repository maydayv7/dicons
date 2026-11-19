#pragma once

#include "gobject_ptr.hpp"
#include <gtk/gtk.h>

struct DesktopSort
{
    DesktopSort();

    ObjectPtr<GSimpleActionGroup> actions;
    ObjectPtr<GtkCustomSorter> sorter;
};

void append_sort_menu(GMenu *menu);
