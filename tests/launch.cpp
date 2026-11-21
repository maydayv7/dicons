#include "launch.hpp"
#include "gobject_ptr.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>
#include <unistd.h>

namespace fs = std::filesystem;

static int probe(const fs::path &folder, std::string_view argument)
{
    std::ofstream(folder / "pending") << getsid(0) << '\n'
                                      << argument << '\n';
    fs::rename(folder / "pending", folder / "result");
    return 0;
}

static void assert_launch(const fs::path &folder, const std::string &expected)
{
    auto path = folder / "result";
    auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!fs::exists(path) && g_get_monotonic_time() < deadline)
    {
        g_main_context_iteration(nullptr, false);
        g_usleep(1000);
    }
    std::ifstream result(path);
    std::string session, argument;
    g_assert_true(bool(std::getline(result, session)));
    std::getline(result, argument);
    g_assert_cmpint(std::stol(session), !=, getsid(0));
    g_assert_true(argument == expected);
}

static void desktop_app()
{
    fs::path folder = fs::path(g_get_user_data_dir()) / "a folder";
    fs::create_directories(folder);
    auto command = "\"" + fs::canonical("/proc/self/exe").string() + "\" --probe \"" + folder.string() + "\" %u";
    g_autoptr(GKeyFile) entry = g_key_file_new();
    g_key_file_set_string(entry, "Desktop Entry", "Type", "Application");
    g_key_file_set_string(entry, "Desktop Entry", "Name", "Launch Test");
    g_key_file_set_string(entry, "Desktop Entry", "Exec", command.c_str());
    ObjectPtr<GDesktopAppInfo> handler{g_desktop_app_info_new_from_keyfile(entry)};
    g_assert_nonnull(handler);
    char uri[] = "trash:///";
    GList uris{uri};
    for (auto *arguments : {static_cast<GList *>(nullptr), &uris})
    {
        g_autoptr(GError) error = nullptr;
        g_assert_true(launch_desktop_app(handler.get(), arguments, nullptr, &error));
        g_assert_no_error(error);
        assert_launch(folder, arguments ? uri : "");
        fs::remove(folder / "result");
    }
}

static void terminal()
{
    fs::path folder = fs::path(g_get_user_data_dir()) / "terminal folder";
    fs::create_directories(folder);
    fs::create_symlink(fs::canonical("/proc/self/exe"), folder / "xdg-terminal-exec");
    std::string path = folder.string() + ":" + g_getenv("PATH");
    g_setenv("PATH", path.c_str(), true);
    ObjectPtr<GtkApplication> app{gtk_application_new(nullptr, G_APPLICATION_NON_UNIQUE)};
    ObjectPtr<GFile> directory{g_file_new_for_path(folder.c_str())};
    open_terminal(app.get(), directory.get());
    assert_launch(folder, "--dir=" + folder.string());
}

int main(int argc, char **argv)
{
    if (argc >= 3 && std::string_view(argv[1]) == "--probe")
        return probe(argv[2], argc == 4 ? argv[3] : "");
    if (argc == 2 && std::string_view(argv[1]).starts_with("--dir="))
        return probe(fs::current_path(), argv[1]);
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, nullptr);
    g_test_add_func("/launch/terminal", terminal);
    g_test_add_func("/launch/desktop-app", desktop_app);
    return g_test_run();
}
