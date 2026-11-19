#pragma once

#include <gtk/gtk.h>

GtkWidget *create_item_icon(GSimpleAction *show_thumbnails);
void bind_item_icon(GtkWidget *widget, GFileInfo *info);
void unbind_item_icon(GtkWidget *widget);
