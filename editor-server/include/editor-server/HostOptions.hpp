#pragma once

#include <atomic>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <math/UUID.hpp>

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
        /// Stop when stdin reaches end of file or fails to read (StartStdinEofWatcher): a launcher that dies before its
        /// client's first Hello leaves nothing else to end the host, but its stdin pipe closes. Opt-in, since a
        /// launcher that closes or doesn't provide stdin would otherwise kill the host at once.
        bool exitOnStdinEof = false;
        bool showHelp = false;

        /// --create: make this folder a project (IO::CreateProject), print the created line, and exit; the engine
        /// isn't started. Empty: open --project (or none) as usual.
        std::string createPath;
        /// --name, with --create: the new project's name (empty: the folder's name)
        std::string projectName;
        /// --project-id <uuid>, with --create: the new project's id (nullopt: a random one, or see projectIdFromPath)
        std::optional<Math::UUID> projectId;
        /// --project-id from-path, with --create: the id is the namespace the folder's asset UUIDs had before projects
        /// had ids (ResourceUUID::NamespaceForProjectDir), so adopting a folder keeps every UUID it has
        bool projectIdFromPath = false;
    };

    /// The value of --project-id that asks for ResourceUUID::NamespaceForProjectDir of the --create folder
    inline constexpr std::string_view ProjectIdFromPath = "from-path";

    /// N2EditorHost --create's exit code when the folder already has a project.n2proj (nothing is written). 0 is
    /// success and 1 any other failure.
    inline constexpr int ExitCodeAlreadyAProject = 2;

    /// Starts the line N2EditorHost --create prints to stdout once the project exists
    inline constexpr std::string_view CreatedLinePrefix = "N2EditorHost created";

    /// The created line, without the newline: "N2EditorHost created projectId=<uuid> startupScene=<res path>". As
    /// with the ready line, a launcher matches the prefix, then space-separated key=value fields (values never hold a
    /// space), ignoring keys it doesn't know; projectId is always present, startupScene when the project has one.
    [[nodiscard]] std::string FormatCreatedLine(const Math::UUID &projectId, std::string_view startupScene);

    /// A decimal port in [0, 65535], nothing else (no sign, whitespace or trailing text). 0 means "any free
    /// port" (EditorServer::Start(0)).
    [[nodiscard]] std::optional<int> ParsePort(std::string_view text);

    /// "opengl" or "software" (exactly; no other spelling)
    [[nodiscard]] std::optional<HostRenderer> ParseRenderer(std::string_view text);

    /// The arguments after the program name. -h/--help anywhere wins: the result is the defaults with showHelp
    /// set, whatever else is there. Otherwise an option missing its value (none follows, or the next argument
    /// starts with "--"), an invalid port, renderer or --project-id, --create together with --project, and --name or
    /// --project-id without --create are errors (the message names the option); unknown arguments are ignored.
    [[nodiscard]] std::expected<HostOptions, std::string> ParseHostArguments(const std::vector<std::string> &args);

    /// Reads up to `size` bytes into `buffer`; returns the count read, 0 at end of file, and a negative number on error
    using StdinReader = std::function<std::ptrdiff_t(char *buffer, std::size_t size)>;

    /// --exit-on-stdin-eof's loop: reads with `readSome` and discards what it gets, until it reports end of file (0) or
    /// an error (negative), then returns
    void DrainUntilEof(const StdinReader &readSome);

    /**
     * --exit-on-stdin-eof: starts a detached thread that drains this process's stdin (DrainUntilEof over a raw
     * ReadFile/read on the OS handle, never the CRT's stdin, so the exit of the process can't wait on its lock) and, at
     * end of file or on any read error (a closed or invalid stdin included), sets the returned flag. The caller
     * polls the flag in its main loop. The thread is detached because a read on a pipe that stays open can't be
     * interrupted; it only holds the flag, so it is harmless once the host has returned, and ends with the process.
     *
     * The watcher starts once per process (a later call returns the same flag, already true if stdin has closed): the
     * flag hands stdin to the host for the life of the process, and whatever stdin held is discarded. Give it a pipe or
     * a file, not a terminal: a POSIX background process reading its terminal gets SIGTTIN, and a Windows console
     * discards the lines typed into it (Ctrl+Z then Enter is its end of file).
     */
    [[nodiscard]] std::shared_ptr<std::atomic<bool>> StartStdinEofWatcher();

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
