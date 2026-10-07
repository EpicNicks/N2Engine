#include "editor-server/HostOptions.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <utility>

namespace N2Engine::Editor
{
    std::optional<int> ParsePort(const std::string_view text)
    {
        // from_chars takes "-0" as 0; a port has no sign
        if (text.empty() || text.front() == '-')
        {
            return std::nullopt;
        }
        int value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < 0 || value > 65535)
        {
            return std::nullopt;
        }
        return value;
    }

    std::optional<HostRenderer> ParseRenderer(const std::string_view text)
    {
        if (text == "opengl")
        {
            return HostRenderer::OpenGL;
        }
        if (text == "software")
        {
            return HostRenderer::Software;
        }
        return std::nullopt;
    }

    std::expected<HostOptions, std::string> ParseHostArguments(const std::vector<std::string> &args)
    {
        HostOptions options;

        // Help wins: "N2EditorHost --port x --help" shows the usage instead of complaining about the port
        for (const std::string &arg : args)
        {
            if (arg == "-h" || arg == "--help")
            {
                options.showHelp = true;
                return options;
            }
        }

        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string &arg = args[i];
            if (arg == "--exit-on-disconnect")
            {
                options.exitOnDisconnect = true;
                continue;
            }
            const bool takesValue = arg == "-p" || arg == "--port" || arg == "--bind" || arg == "--project" ||
                                    arg == "--renderer" || arg == "--token-env";
            if (!takesValue)
            {
                continue; // unknown arguments are ignored
            }
            // "--project --port 0" is a forgotten path, not a project folder called "--port"
            if (i + 1 >= args.size() || args[i + 1].starts_with("--"))
            {
                return std::unexpected(arg + " is missing a value");
            }
            const std::string &value = args[++i];

            if (arg == "-p" || arg == "--port")
            {
                const std::optional<int> port = ParsePort(value);
                if (!port)
                {
                    return std::unexpected("Invalid port: " + value);
                }
                options.port = *port;
            }
            else if (arg == "--bind")
            {
                options.bindAddress = value;
            }
            else if (arg == "--renderer")
            {
                const std::optional<HostRenderer> renderer = ParseRenderer(value);
                if (!renderer)
                {
                    return std::unexpected("Invalid renderer: " + value + " (expected opengl or software)");
                }
                options.renderer = *renderer;
            }
            else if (arg == "--token-env")
            {
                // The variable's name; the token is read from it at startup (ReadAccessToken), not here
                options.tokenEnv = value;
            }
            else
            {
                options.projectPath = value;
            }
        }
        return options;
    }

    std::string_view HostUsage()
    {
        return R"(N2Engine Editor Host
Usage: N2EditorHost [options]
Options:
  -p, --port <port>         Server port (default: 9999; 0 lets the OS pick a free one)
  --bind <ipv4 address>     Address to listen on (default: 127.0.0.1)
                            WARNING: the access token isn't encrypted; any other address lets whoever
                            can reach it (and, without --token-env, anyone at all) control this host,
                            including deleting scene files
  --project <path>          Project folder: res:// assets load from <path>/assets,
                            and DeleteScene works in <path>/scenes
  --renderer <name>         opengl (default: a hidden window, needs a GPU) or
                            software (no window at all; renders without a GPU or display)
  --token-env <variable>    Read the access token from this environment variable (then removed
                            from the host's environment): every connection must send Hello with it
                            before any other command
  --exit-on-disconnect      Exit once a client's session ends (its connection closes after a
                            successful Hello, or with no token, after it connected)
  -h, --help                Show this help
Once listening, prints the line "N2EditorHost ready port=<port>" to stdout.
)";
    }

    std::expected<std::string, std::string> ReadAccessToken(const std::string &variable)
    {
        if (variable.empty() || variable.find('=') != std::string::npos)
        {
            return std::unexpected("--token-env: '" + variable + "' is not an environment variable name");
        }

        std::optional<std::string> value;
#ifdef _WIN32
        // _dupenv_s, not getenv (which MSVC deprecates); an unset variable succeeds with a null buffer
        char *buffer = nullptr;
        size_t length = 0;
        if (_dupenv_s(&buffer, &length, variable.c_str()) == 0 && buffer != nullptr)
        {
            value = buffer;
        }
        std::free(buffer);
#else
        if (const char *text = std::getenv(variable.c_str()); text != nullptr)
        {
            value = text;
        }
#endif
        if (!value)
        {
            return std::unexpected("--token-env: the environment variable " + variable + " is not set");
        }

        // Out of the environment, whatever it held: Lua, or anything the host starts, can't read it from there
#ifdef _WIN32
        (void)_putenv_s(variable.c_str(), ""); // an empty value removes the variable
#else
        (void)unsetenv(variable.c_str());
#endif

        if (value->empty())
        {
            return std::unexpected("--token-env: the environment variable " + variable + " is empty");
        }
        return std::move(*value);
    }

    std::string FormatReadyLine(const int port)
    {
        return std::format("{} port={}", ReadyLinePrefix, port);
    }
}
