#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Editor
{
    /// N2EditorHost's command line, parsed. Kept in the library (not Main.cpp) so it can be unit tested.
    struct HostOptions
    {
        static constexpr int DefaultPort = 9999;

        /// 0 lets the OS pick a free port; the ready line reports the one it picked
        int port = DefaultPort;
        /// EditorServer::DefaultBindAddress, spelled out to keep EditorServer.hpp out of this header;
        /// HostOptionsTest.NoArgumentsGiveTheDefaults pins the two equal
        std::string bindAddress = "127.0.0.1";
        /// Empty: no project, so no ResourceLoader/ResourceUUID (asset references don't resolve)
        std::string projectPath;
        bool showHelp = false;
    };

    /// A decimal port in [0, 65535], nothing else (no sign, whitespace or trailing text). 0 means "any free
    /// port" (EditorServer::Start(0)).
    [[nodiscard]] std::optional<int> ParsePort(std::string_view text);

    /// The arguments after the program name. -h/--help anywhere wins: the result is the defaults with showHelp
    /// set, whatever else is there. Otherwise an option missing its value (none follows, or the next argument
    /// starts with "--") or an invalid port is an error (the message names it); unknown arguments are ignored.
    [[nodiscard]] std::expected<HostOptions, std::string> ParseHostArguments(const std::vector<std::string> &args);

    /// Starts the line N2EditorHost prints to stdout once the server is listening
    inline constexpr std::string_view ReadyLinePrefix = "N2EditorHost ready";

    /// The ready line, without the newline: "N2EditorHost ready port=<port>". Launchers match a whole line
    /// starting with ReadyLinePrefix followed by space-separated key=value fields; port is always present and
    /// later fields may be appended, so a parser must ignore keys it doesn't know.
    [[nodiscard]] std::string FormatReadyLine(int port);
}
