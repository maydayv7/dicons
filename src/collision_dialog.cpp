#include "collision_dialog.hpp"
#include "gobject_ptr.hpp"

#include <utility>

void choose_collision_action(GtkApplication *app, GtkWindow *parent, const char *path,
                             CollisionCallback callback)
{
    if (!parent)
        parent = gtk_application_get_active_window(app);
    constexpr auto message = "An item with this name already exists.";
    auto choice = std::make_unique<CollisionCallback>(std::move(callback));
    const char *buttons[] = {"Skip", "Keep Both", nullptr};
    ObjectPtr<GtkAlertDialog> dialog{gtk_alert_dialog_new("%s", message)};
    gtk_alert_dialog_set_detail(dialog.get(), path);
    gtk_alert_dialog_set_buttons(dialog.get(), buttons);
    gtk_alert_dialog_set_cancel_button(dialog.get(), 0);
    gtk_alert_dialog_choose(dialog.get(), parent, nullptr, [](GObject *source, GAsyncResult *result, gpointer data)
                            {
        std::unique_ptr<CollisionCallback> done{static_cast<CollisionCallback *>(data)};
        const int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr);
        (*done)(button == 1 ? CollisionChoice::keep_both : CollisionChoice::skip); }, choice.release());
}
