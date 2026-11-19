#pragma once

#include <gtk/gtk.h>

struct DesktopDirectory;
enum class NewItem
{
    folder,
    blank,
    from_template
};
void create_item(GtkGridView *grid, const DesktopDirectory &directory, NewItem kind, GFile *source = nullptr);
