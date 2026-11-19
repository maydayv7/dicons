#include "desktop_directory.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

static guint position_of(GListModel *model, std::string_view name)
{
    for (guint i = 0; i < g_list_model_get_n_items(model); ++i)
    {
        ObjectPtr<GFileInfo> info{G_FILE_INFO(g_list_model_get_item(model, i))};
        if (g_file_info_get_name(info.get()) == name)
            return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

static bool wait_for(auto predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do
    {
        while (g_main_context_iteration(nullptr, false))
        {
        }
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static void atomic_saves()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / "selected.txt") << "keep selected";
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    DesktopDirectory directory{folder.get(), app.get()};
    const auto selection = [&directory]
    {
        return ObjectPtr<GtkMultiSelection>{gtk_multi_selection_new(g_object_ref(directory.items.get()))};
    };
    const auto first = selection();
    const auto second = selection();
    auto *left = G_LIST_MODEL(first.get());
    auto *right = G_LIST_MODEL(second.get());

    // Race both the initial enumeration and later metadata lookups
    std::ofstream(root / "initial.tmp") << "initial";
    fs::rename(root / "initial.tmp", root / "edited.txt");
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(left) == 2 && position_of(left, "selected.txt") != GTK_INVALID_LIST_POSITION &&
                                    position_of(left, "edited.txt") != GTK_INVALID_LIST_POSITION; }));
    gtk_selection_model_select_item(GTK_SELECTION_MODEL(first.get()), position_of(left, "selected.txt"), true);

    for (int i = 0; i < 32; ++i)
    {
        std::ofstream(root / "editor.tmp") << i;
        fs::rename(root / "editor.tmp", root / "edited.txt");
    }
    fs::remove(root / "edited.txt");
    std::ofstream(root / "after.txt") << "later change";
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(left) == 2 && position_of(left, "after.txt") != GTK_INVALID_LIST_POSITION &&
                                    position_of(right, "after.txt") != GTK_INVALID_LIST_POSITION; }));
    fs::remove(root / "after.txt");
    g_assert_true(wait_for([&]
                           { return g_list_model_get_n_items(left) == 1 && g_list_model_get_n_items(right) == 1 &&
                                    position_of(left, "selected.txt") == 0 && position_of(right, "selected.txt") == 0; }));
    g_assert_true(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(first.get()), 0));
    g_assert_false(gtk_selection_model_is_selected(GTK_SELECTION_MODEL(second.get()), 0));
}

static void desktop_folder_events()
{
    const auto root = fs::path(g_get_user_data_dir()) / "Desktop";
    fs::create_directories(root);
    std::ofstream(root / "first") << "first";
    ObjectPtr<GFile> folder{g_file_new_for_path(root.c_str())};
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    DesktopDirectory directory{folder.get(), app.get()};
    auto *items = directory.items.get();
    g_assert_true(wait_for([&]
                           { return position_of(items, "first") != GTK_INVALID_LIST_POSITION; }));

    fs::permissions(root, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
    std::ofstream(root / "second") << "second";
    g_assert_true(wait_for([&]
                           { return position_of(items, "second") != GTK_INVALID_LIST_POSITION; }));
    g_assert_cmpuint(g_list_model_get_n_items(items), ==, 2);
    g_assert_cmpuint(position_of(items, "Desktop"), ==, GTK_INVALID_LIST_POSITION);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/directory/atomic-saves", atomic_saves);
    g_test_add_func("/directory/desktop-folder-events", desktop_folder_events);
    return g_test_run();
}
