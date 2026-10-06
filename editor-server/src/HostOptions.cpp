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
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string &arg = args[i];
            const bool hasValue = i + 1 < args.size();
            if (arg == "-p" || arg == "--port")
            {
                if (!hasValue)
                {
                    return std::unexpected(arg + " needs a port");
                }
                const std::optional<int> port = ParsePort(args[++i]);
                if (!port)
                {
                    return std::unexpected("Invalid port: " + args[i]);
                }
                options.port = *port;
            }
            else if (arg == "--bind")
            {
                if (!hasValue)
                {
                    return std::unexpected(arg + " needs an address");
                }
                options.bindAddress = args[++i];
            }
            else if (arg == "--project")
            {
                if (!hasValue)
                {
                    return std::unexpected(arg + " needs a path");
                }
                options.projectPath = args[++i];
            }
            else if (arg == "-h" || arg == "--help")
            {
                options.showHelp = true;
            }
        }
        return options;
    }

    std::string FormatReadyLine(const int port)
    {
        return std::format("{} port={}", ReadyLinePrefix, port);
    }
}
