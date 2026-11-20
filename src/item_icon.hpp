#pragma once

#include <gtk/gtk.h>

GtkWidget *create_item_icon(GAction *show_thumbnails, GSettings *preferences);
void bind_item_icon(GtkWidget *widget, GFileInfo *info);
void unbind_item_icon(GtkWidget *widget);
