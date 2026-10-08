#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace N2Engine::Editor
{
    /// The renderer --renderer chooses
    enum class HostRenderer
    {
        /// A hidden GLFW window with an OpenGL context (the default); fails on a machine without a GPU
        OpenGL,
        /// The CPU renderer with no window at all (Window::UsesNoWindow), so frames render anywhere
        Software,
    };

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
        HostRenderer renderer = HostRenderer::OpenGL;
        /// The environment variable holding the access token (ReadAccessToken), so the token itself never appears on
        /// a command line. Empty: the host has no access token, and Hello is optional.
        std::string tokenEnv;
        /// Stop once a client's session ends (EditorServer::SetStopOnDisconnect), so a host whose launcher has gone
        /// doesn't keep running
        bool exitOnDisconnect = false;
        bool showHelp = false;
    };

    /// A decimal port in [0, 65535], nothing else (no sign, whitespace or trailing text). 0 means "any free
    /// port" (EditorServer::Start(0)).
    [[nodiscard]] std::optional<int> ParsePort(std::string_view text);

    /// "opengl" or "software" (exactly; no other spelling)
    [[nodiscard]] std::optional<HostRenderer> ParseRenderer(std::string_view text);

    /// The arguments after the program name. -h/--help anywhere wins: the result is the defaults with showHelp
    /// set, whatever else is there. Otherwise an option missing its value (none follows, or the next argument
    /// starts with "--") or an invalid port is an error (the message names it); unknown arguments are ignored.
    [[nodiscard]] std::expected<HostOptions, std::string> ParseHostArguments(const std::vector<std::string> &args);

    /// N2EditorHost's usage text (what --help prints), one option per line
    [[nodiscard]] std::string_view HostUsage();

    /**
     * --token-env: the access token held by the environment variable `variable`, which is then removed from this
     * process's environment, so nothing the host later runs or starts can read it. An error (naming the variable,
     * never the value) when the name is empty or contains '=', when the variable isn't set, or when it is empty: a
     * launcher that asked for a token must not get a host that silently has none.
     */
    [[nodiscard]] std::expected<std::string, std::string> ReadAccessToken(const std::string &variable);

    /// Starts the line N2EditorHost prints to stdout once the server is listening
    inline constexpr std::string_view ReadyLinePrefix = "N2EditorHost ready";

    /// The ready line, without the newline: "N2EditorHost ready port=<port>". Launchers match a whole line
    /// starting with ReadyLinePrefix followed by space-separated key=value fields; port is always present and
    /// later fields may be appended, so a parser must ignore keys it doesn't know.
    [[nodiscard]] std::string FormatReadyLine(int port);
}
