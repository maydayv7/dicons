#include "desktop_directory.hpp"
#include "desktop_windows.hpp"
#include "preferences.hpp"
#include "undo.hpp"

#include <gtk4-layer-shell.h>
#include <cstdlib>
#include <optional>

namespace
{
    class DesktopApplication
    {
    public:
        int run(int argc, char **argv)
        {
            g_signal_connect(app_.get(), "activate", G_CALLBACK(on_activate), this);
            const int result = g_application_run(G_APPLICATION(app_.get()), argc, argv);
            if (monitors_)
                g_signal_handlers_disconnect_by_data(monitors_.get(), this);
            return exit_status_ ? exit_status_ : result;
        }

    private:
        static void on_activate(GtkApplication *, DesktopApplication *self) { self->activate(); }

        static void monitors_changed(GListModel *, guint, guint, guint, DesktopApplication *self)
        {
            sync_windows(self->app_.get(), self->monitors_.get(), *self->directory_);
        }

        void activate()
        {
            if (directory_)
            {
                sync_windows(app_.get(), monitors_.get(), *directory_);
                return;
            }
            if (!gtk_layer_is_supported())
            {
                g_printerr("dicons requires a Wayland compositor with layer-shell support.\n");
                exit_status_ = EXIT_FAILURE;
                return;
            }

            auto preferences = desktop_preferences();
            g_autoptr(GError) error = nullptr;
            auto folder = open_desktop_folder(preferences.get(), &error);
            if (!folder)
            {
                if (error)
                {
                    g_printerr("%s\n", error->message);
                    exit_status_ = EXIT_FAILURE;
                }
                return;
            }

            attach_undo_preferences(G_APPLICATION(app_.get()), preferences.get(), folder.get());
            directory_.emplace(folder.get(), app_.get(), preferences.get());
            auto *display = gdk_display_get_default();
            install_desktop_style(display);
            monitors_ = retain(gdk_display_get_monitors(display));
            // Keep watching for outputs when the last monitor disconnects
            g_application_hold(G_APPLICATION(app_.get()));
            g_signal_connect(monitors_.get(), "items-changed", G_CALLBACK(monitors_changed), this);
            sync_windows(app_.get(), monitors_.get(), *directory_);
        }

        int exit_status_{};
        std::optional<DesktopDirectory> directory_;
        ObjectPtr<GListModel> monitors_;
        ObjectPtr<GtkApplication> app_{gtk_application_new(desktop_app_id, G_APPLICATION_DEFAULT_FLAGS)};
    };
}

#ifdef DICONS_APPIMAGE
void init_appimage();
#endif

int main(int argc, char **argv)
{
#ifdef DICONS_APPIMAGE
    init_appimage();
#endif
    g_set_prgname(desktop_app_id);
    return DesktopApplication{}.run(argc, argv);
}
