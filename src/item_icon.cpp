#include "item_icon.hpp"
#include "desktop_directory.hpp"
#include "thumbnail_cache.hpp"

#include <optional>
#include <set>

namespace
{
    struct Icon;

    struct Preview
    {
        ObjectPtr<GCancellable> cancel{g_cancellable_new()};
        ObjectPtr<GdkTexture> texture;
        std::set<Icon *> viewers;
        ~Preview() { g_cancellable_cancel(cancel.get()); }
    };

    struct Icon
    {
        GtkImage *base;
        GtkPicture *picture;
        GtkImage *badge;
        ObjectPtr<GSimpleAction> enabled;
        ObjectPtr<GFileInfo> item;
        std::shared_ptr<Preview> preview;

        void detach()
        {
            if (preview)
                preview->viewers.erase(this);
            preview.reset();
        }
        ~Icon() { detach(); }

        void display()
        {
            auto *texture = preview ? preview->texture.get() : nullptr;
            gtk_picture_set_paintable(picture, texture ? GDK_PAINTABLE(texture) : nullptr);
            gtk_widget_set_opacity(GTK_WIDGET(base), texture ? 0.0 : 1.0);
        }
    };

    struct History
    {
        std::optional<ThumbnailVersion> version;
        std::weak_ptr<Preview> preview;
        bool changed_within_second = false;
    };

    struct Job
    {
        std::weak_ptr<Preview> preview;
        ThumbnailVersion version;
        bool changed_within_second;
    };

    void load(GTask *task, gpointer file, gpointer data, GCancellable *cancel)
    {
        auto &job = *static_cast<Job *>(data);
        auto image = load_thumbnail(G_FILE(file), job.version, cancel, job.changed_within_second);
        auto *pixels = image.release();
        g_task_return_pointer(task, pixels, pixels ? g_object_unref : nullptr);
    }

    void loaded(GObject *, GAsyncResult *result, gpointer)
    {
        auto *task = G_TASK(result);
        auto &job = *static_cast<Job *>(g_task_get_task_data(task));
        ObjectPtr<GdkPixbuf> image{static_cast<GdkPixbuf *>(g_task_propagate_pointer(task, nullptr))};
        if (auto preview = job.preview.lock(); preview && image)
        {
            g_autoptr(GBytes) pixels = gdk_pixbuf_read_pixel_bytes(image.get());
            auto format = gdk_pixbuf_get_has_alpha(image.get()) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
            preview->texture.reset(gdk_memory_texture_new(gdk_pixbuf_get_width(image.get()), gdk_pixbuf_get_height(image.get()),
                                                          format, pixels, gdk_pixbuf_get_rowstride(image.get())));
            for (auto *icon : preview->viewers)
                icon->display();
        }
    }

    std::shared_ptr<Preview> request(GFileInfo *info)
    {
        auto *history = static_cast<History *>(g_object_get_data(G_OBJECT(info), "thumbnail-history"));
        if (!history)
        {
            history = new History;
            g_object_set_data_full(G_OBJECT(info), "thumbnail-history", history,
                                   [](gpointer p)
                                   { delete static_cast<History *>(p); });
        }
        ThumbnailVersion version(info);
        if (history->version == version)
        {
            if (auto current = history->preview.lock())
                return current;
        }
        else
        {
            history->changed_within_second = history->version && history->version->modified == version.modified;
            history->version = version;
        }
        auto preview = std::make_shared<Preview>();
        history->preview = preview;
        auto source = version.target_path.empty() ? retain(item_file(info)) : ObjectPtr<GFile>{g_file_new_for_path(version.target_path.c_str())};
        ObjectPtr<GTask> task{g_task_new(source.get(), preview->cancel.get(), loaded, nullptr)};
        g_task_set_task_data(task.get(), new Job{preview, std::move(version), history->changed_within_second},
                             [](gpointer p)
                             { delete static_cast<Job *>(p); });
        g_task_run_in_thread(task.get(), load);
        return preview;
    }

    void update_preview(Icon &icon)
    {
        g_autoptr(GVariant) enabled = g_action_get_state(G_ACTION(icon.enabled.get()));
        std::shared_ptr<Preview> preview;
        if (icon.item && g_variant_get_boolean(enabled) && can_preview(icon.item.get()))
            preview = request(icon.item.get());
        if (preview != icon.preview)
        {
            icon.detach();
            icon.preview = std::move(preview);
            if (icon.preview)
                icon.preview->viewers.insert(&icon);
        }
        icon.display();
    }

    Icon &icon_for(GtkWidget *widget)
    {
        return *static_cast<Icon *>(g_object_get_data(G_OBJECT(widget), "item-icon"));
    }

    void preference_changed(GSimpleAction *, GParamSpec *, GtkWidget *widget)
    {
        update_preview(icon_for(widget));
    }
}

GtkWidget *create_item_icon(GSimpleAction *show_thumbnails)
{
    auto *overlay = gtk_overlay_new();
    auto *base = gtk_image_new();
    auto *picture = gtk_picture_new();
    auto *badge = gtk_image_new_from_icon_name("emblem-symbolic-link");
    gtk_image_set_pixel_size(GTK_IMAGE(base), 48);
    gtk_widget_set_size_request(base, 48, 48);
    gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_image_set_pixel_size(GTK_IMAGE(badge), 16);
    gtk_widget_set_halign(badge, GTK_ALIGN_START);
    gtk_widget_set_valign(badge, GTK_ALIGN_END);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), base);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), picture);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badge);
    auto *icon = new Icon{GTK_IMAGE(base), GTK_PICTURE(picture), GTK_IMAGE(badge), retain(show_thumbnails)};
    g_object_set_data_full(G_OBJECT(overlay), "item-icon", icon, [](gpointer p)
                           { delete static_cast<Icon *>(p); });
    g_signal_connect_object(show_thumbnails, "notify::state", G_CALLBACK(preference_changed), overlay, GConnectFlags{});
    return overlay;
}

void bind_item_icon(GtkWidget *widget, GFileInfo *info)
{
    auto &icon = icon_for(widget);
    icon.item = retain(info);
    bool link = g_file_info_get_is_symlink(info);
    bool broken = link && g_file_info_get_file_type(info) == G_FILE_TYPE_SYMBOLIC_LINK;
    if (broken)
        gtk_image_set_from_icon_name(icon.base, "dialog-warning-symbolic");
    else if (auto *base = g_file_info_get_icon(info))
    {
        if (G_IS_THEMED_ICON(base))
        {
            auto names = g_themed_icon_get_names(G_THEMED_ICON(base));
            g_autoptr(GIcon) themed = g_themed_icon_new(names[0]);
            for (auto name = names + 1; *name; ++name)
                g_themed_icon_append_name(G_THEMED_ICON(themed), *name);
            g_themed_icon_append_name(G_THEMED_ICON(themed), "text-x-generic");
            g_themed_icon_append_name(G_THEMED_ICON(themed), "text-x-generic-symbolic");
            gtk_image_set_from_gicon(icon.base, themed);
        }
        else
            gtk_image_set_from_gicon(icon.base, base);
    }
    else
        gtk_image_set_from_icon_name(icon.base, "text-x-generic");
    bool trash = is_trash_icon(info);
    bool full = trash && g_file_info_get_attribute_boolean(info, "dicons::full");
    // Minimal themes ship only empty icon
    auto *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(widget));
    bool mark_full = full && !gtk_icon_theme_has_icon(theme, "user-trash-full") &&
                     !gtk_icon_theme_has_icon(theme, "user-trash-full-symbolic");
    auto *badge = GTK_WIDGET(icon.badge);
    if (mark_full || link)
    {
        gtk_image_set_from_icon_name(icon.badge, mark_full ? "list-add-symbolic" : "emblem-symbolic-link");
        gtk_widget_set_halign(badge, mark_full ? GTK_ALIGN_END : GTK_ALIGN_START);
        if (mark_full)
            gtk_widget_add_css_class(badge, "osd");
        else
            gtk_widget_remove_css_class(badge, "osd");
    }
    gtk_widget_set_visible(badge, link || mark_full);
    const char *tooltip = broken ? "Broken symbolic link" : (link ? "Symbolic link" : nullptr);
    gtk_widget_set_tooltip_text(widget, trash ? (full ? "Trash contains items" : "Trash is empty") : tooltip);
    update_preview(icon);
}

void unbind_item_icon(GtkWidget *widget)
{
    auto &icon = icon_for(widget);
    icon.item.reset();
    icon.detach();
    icon.display();
}
