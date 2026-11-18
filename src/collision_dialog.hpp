#pragma once

#include <gtk/gtk.h>
#include <functional>

enum class CollisionChoice
{
    skip,
    keep_both
};
using CollisionCallback = std::function<void(CollisionChoice)>;

void choose_collision_action(GtkApplication *app, GtkWindow *parent, const char *path,
                             CollisionCallback callback);
