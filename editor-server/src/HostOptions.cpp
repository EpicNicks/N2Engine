#include "editor-server/HostOptions.hpp"

#include <charconv>
#include <format>

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
            const bool takesValue = arg == "-p" || arg == "--port" || arg == "--bind" || arg == "--project";
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
            else
            {
                options.projectPath = value;
            }
        }
        return options;
    }

    std::string FormatReadyLine(const int port)
    {
        return std::format("{} port={}", ReadyLinePrefix, port);
    }
}
