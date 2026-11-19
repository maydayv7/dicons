#include "thumbnailer.hpp"

#include <glib/gstdio.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    using Commands = std::map<std::string, std::string, std::less<>>;

    void read_directory(const char *base, Commands &commands, std::set<std::string> &seen)
    {
        g_autofree char *path = g_build_filename(base, "thumbnailers", nullptr);
        g_autoptr(GDir) dir = g_dir_open(path, 0, nullptr);
        if (!dir)
            return;
        std::vector<std::string> names;
        while (const char *name = g_dir_read_name(dir))
            if (g_str_has_suffix(name, ".thumbnailer"))
                names.emplace_back(name);
        std::ranges::sort(names);
        for (const auto &name : names)
        {
            if (!seen.insert(name).second)
                continue;
            g_autofree char *entry = g_build_filename(path, name.c_str(), nullptr);
            g_autoptr(GKeyFile) keys = g_key_file_new();
            if (!g_key_file_load_from_file(keys, entry, G_KEY_FILE_NONE, nullptr))
                continue;
            constexpr auto group = "Thumbnailer Entry";
            g_autofree char *program = g_key_file_get_string(keys, group, "TryExec", nullptr);
            g_autofree char *executable = program ? g_find_program_in_path(program) : nullptr;
            if (program && !executable)
                continue;
            g_autofree char *command = g_key_file_get_string(keys, group, "Exec", nullptr);
            g_auto(GStrv) types = g_key_file_get_string_list(keys, group, "MimeType", nullptr, nullptr);
            for (auto **type = types; command && type && *type; ++type)
                commands.try_emplace(*type, command);
        }
    }

    Commands installed_commands()
    {
        Commands commands;
        std::set<std::string> seen;
        read_directory(g_get_user_data_dir(), commands, seen);
        for (auto dir = g_get_system_data_dirs(); *dir; ++dir)
            read_directory(*dir, commands, seen);
        return commands;
    }

    ObjectPtr<GSubprocess> launch(const char *command, GFile *file, const char *output, int size)
    {
        g_auto(GStrv) tokens = nullptr;
        if (!g_shell_parse_argv(command, nullptr, &tokens, nullptr))
            return {};
        g_autofree char *uri = g_file_get_uri(file);
        std::vector<std::string> arguments;
        for (auto **token = tokens; *token; ++token)
        {
            auto &arg = arguments.emplace_back();
            for (const char *c = *token; *c; ++c)
            {
                if (*c != '%')
                    arg += *c;
                else
                    switch (*++c)
                    {
                    case 'u':
                        arg += uri;
                        break;
                    case 'i':
                        arg += g_file_peek_path(file);
                        break;
                    case 'o':
                        arg += output;
                        break;
                    case 's':
                        arg += std::to_string(size);
                        break;
                    case '%':
                        arg += '%';
                        break;
                    default:
                        return {};
                    }
            }
        }
        std::vector<const char *> argv;
        for (const auto &arg : arguments)
            argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        return ObjectPtr<GSubprocess>{g_subprocess_newv(argv.data(), GSubprocessFlags(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE), nullptr)};
    }

    void stop_helper(GCancellable *, GSubprocess *process) { g_subprocess_force_exit(process); }
}

ObjectPtr<GdkPixbuf> run_thumbnailer(GFile *file, const char *mime, int size, GCancellable *cancel)
{
    static const Commands commands = installed_commands();
    auto it = commands.find(mime);
    if (it == commands.end() || g_cancellable_is_cancelled(cancel))
        return {};
    g_autofree char *dir = g_dir_make_tmp("dicons-thumbnail-XXXXXX", nullptr);
    if (!dir)
        return {};
    g_autofree char *output = g_build_filename(dir, "preview.png", nullptr);
    ObjectPtr<GdkPixbuf> result;
    if (auto process = launch(it->second.c_str(), file, output, size))
    {
        auto handler = g_cancellable_connect(cancel, G_CALLBACK(stop_helper), process.get(), nullptr);
        if (g_subprocess_wait_check(process.get(), nullptr, nullptr) && !g_cancellable_is_cancelled(cancel))
            result.reset(gdk_pixbuf_new_from_file_at_scale(output, size, size, true, nullptr));
        g_cancellable_disconnect(cancel, handler);
    }
    g_unlink(output);
    g_rmdir(dir);
    return result;
}
