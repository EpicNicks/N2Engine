#include "editor-server/HostOptions.hpp"

#include "editor-server/LogSink.hpp"

#include <charconv>
#include <cstdlib>
#include <format>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

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

    std::optional<int> ParseWholeNumber(const std::string_view text, const int min, const int max)
    {
        // from_chars takes a leading '-' (and "-0"); these are durations, which have no sign
        if (text.empty() || text.front() < '0' || text.front() > '9')
        {
            return std::nullopt;
        }
        int value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || value < min || value > max)
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

    bool IsLoopbackBindAddress(const std::string_view text)
    {
        int fields = 0;
        int first = -1;
        size_t pos = 0;
        while (true)
        {
            const size_t end = text.find('.', pos);
            const std::string_view field = text.substr(pos, end == std::string_view::npos ? end : end - pos);
            // 1 to 3 digits, no leading zero ("010" is not 10 to inet_pton, and not a spelling to trust here)
            if (field.empty() || field.size() > 3 || (field.size() > 1 && field.front() == '0'))
            {
                return false;
            }
            int value = 0;
            for (const char c : field)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                value = value * 10 + (c - '0');
            }
            if (value > 255)
            {
                return false;
            }
            if (fields == 0)
            {
                first = value;
            }
            ++fields;
            if (end == std::string_view::npos)
            {
                break;
            }
            pos = end + 1;
        }
        return fields == 4 && first == 127;
    }

    std::expected<HostOptions, std::string> ParseHostArguments(const std::vector<std::string> &args)
    {
        HostOptions options;
        bool givenName = false;
        bool givenId = false;

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
            if (arg == "--exit-on-stdin-eof")
            {
                options.exitOnStdinEof = true;
                continue;
            }
            if (arg == "--tls")
            {
                options.tls = true;
                continue;
            }
            if (arg == "--insecure-bind")
            {
                options.insecureBind = true;
                continue;
            }
            if (arg == "--read-only")
            {
                options.readOnly = true;
                continue;
            }
            if (arg == "--editor-extensions")
            {
                options.editorExtensions = true;
                continue;
            }
            if (arg == "--editor-extensions-watch")
            {
                options.editorExtensionsWatch = true;
                continue;
            }
            if (arg == "--no-rate-limit")
            {
                options.noRateLimit = true;
                continue;
            }
            if (arg == "--new-token")
            {
                // Two values: "--new-token alice editor". A value starting with "--" is the next option.
                if (i + 2 >= args.size() || args[i + 1].starts_with("--") || args[i + 2].starts_with("--"))
                {
                    return std::unexpected("--new-token needs a user and a role (admin, editor or viewer)");
                }
                const std::string &user = args[++i];
                const std::string &roleText = args[++i];
                if (!IsValidTokenUserName(user))
                {
                    return std::unexpected(std::format("--new-token: the user must be 1 to {} characters of "
                                                       "A-Z a-z 0-9 . _ @ -",
                                                       MaxTokenUserLength));
                }
                const std::optional<TokenRole> role = ParseTokenRole(roleText);
                if (!role)
                {
                    return std::unexpected("--new-token: the role must be admin, editor or viewer");
                }
                options.newTokenUser = user;
                options.newTokenRole = *role;
                continue;
            }
            const bool takesValue = arg == "-p" || arg == "--port" || arg == "--bind" || arg == "--project" ||
                                    arg == "--renderer" || arg == "--token-env" || arg == "--tokens-file" ||
                                    arg == "--create" || arg == "--name" || arg == "--project-id" || arg == "--play" ||
                                    arg == "--log-level" || arg == "--log-file" || arg == "--idle-timeout" ||
                                    arg == "--write-timeout" || arg == "--max-session" ||
                                    arg == "--preauth-slots";
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
            else if (arg == "--tokens-file")
            {
                // An empty path would otherwise mean "no tokens file" and leave the host open
                if (value.empty())
                {
                    return std::unexpected("--tokens-file is missing a value");
                }
                options.tokensFile = value;
            }
            else if (arg == "--play")
            {
                options.playScene = value;
            }
            else if (arg == "--idle-timeout")
            {
                const std::optional<int> seconds = ParseWholeNumber(value, 0, HostOptions::MaxIdleTimeoutSeconds);
                if (!seconds)
                {
                    return std::unexpected(std::format("Invalid --idle-timeout: {} (expected whole seconds from 0 to {}; "
                                                       "0 turns it off)",
                                                       value, HostOptions::MaxIdleTimeoutSeconds));
                }
                options.idleTimeoutSeconds = *seconds;
            }
            else if (arg == "--write-timeout")
            {
                const std::optional<int> seconds = ParseWholeNumber(value, 1, HostOptions::MaxWriteTimeoutSeconds);
                if (!seconds)
                {
                    return std::unexpected(std::format("Invalid --write-timeout: {} (expected whole seconds from 1 to {})",
                                                       value, HostOptions::MaxWriteTimeoutSeconds));
                }
                options.writeTimeoutSeconds = *seconds;
            }
            else if (arg == "--max-session")
            {
                const std::optional<int> minutes = ParseWholeNumber(value, 0, HostOptions::MaxSessionMinutesLimit);
                if (!minutes)
                {
                    return std::unexpected(std::format("Invalid --max-session: {} (expected whole minutes from 0 to {}; "
                                                       "0 is off)",
                                                       value, HostOptions::MaxSessionMinutesLimit));
                }
                options.maxSessionMinutes = *minutes;
            }
            else if (arg == "--preauth-slots")
            {
                const std::optional<int> slots = ParseWholeNumber(value, 1, HostOptions::MaxPreAuthSlots);
                if (!slots)
                {
                    return std::unexpected(std::format("Invalid --preauth-slots: {} (expected a whole number from 1 to {})",
                                                       value, HostOptions::MaxPreAuthSlots));
                }
                options.preAuthSlots = *slots;
            }
            else if (arg == "--log-level")
            {
                const std::optional<Logger::LogLevel> level = ParseLogLevel(value);
                if (!level)
                {
                    return std::unexpected("Invalid log level: " + value + " (expected info, warn or error)");
                }
                options.logLevel = *level;
            }
            else if (arg == "--log-file")
            {
                // An empty path would otherwise mean "no file" and silently drop the trail the flag asked for
                if (value.empty())
                {
                    return std::unexpected("--log-file is missing a value");
                }
                options.logFile = value;
            }
            else if (arg == "--create")
            {
                // An empty folder would otherwise mean "don't create" and start a host instead
                if (value.empty())
                {
                    return std::unexpected("--create is missing a value");
                }
                options.createPath = value;
            }
            else if (arg == "--name")
            {
                options.projectName = value;
                givenName = true;
            }
            else if (arg == "--project-id")
            {
                givenId = true;
                if (value == ProjectIdFromPath)
                {
                    options.projectIdFromPath = true;
                    options.projectId.reset();
                    continue;
                }
                const std::optional<Math::UUID> id = Math::UUID::FromString(value);
                if (!id || *id == Math::UUID::ZERO)
                {
                    return std::unexpected("Invalid --project-id: " + value + " (expected a non-zero UUID, or " +
                                           std::string(ProjectIdFromPath) + ")");
                }
                options.projectId = *id;
                options.projectIdFromPath = false;
            }
            else
            {
                options.projectPath = value;
            }
        }

        if (!options.createPath.empty() && !options.projectPath.empty())
        {
            return std::unexpected("--create and --project can't be combined: create the project, then start a host "
                                   "with --project");
        }
        if (!options.createPath.empty() && !options.playScene.empty())
        {
            return std::unexpected("--create and --play can't be combined");
        }
        if (!options.tokensFile.empty() && !options.tokenEnv.empty())
        {
            return std::unexpected("--tokens-file and --token-env can't be combined: one host has one kind of token "
                                   "(--token-env is a single admin token, --tokens-file one token per user)");
        }
        if (!options.newTokenUser.empty() && !options.createPath.empty())
        {
            return std::unexpected("--new-token and --create can't be combined");
        }
        if (options.readOnly && !options.playScene.empty())
        {
            return std::unexpected("--read-only and --play can't be combined: a play host runs the game and the project's "
                                   "Lua scripts, which is not read-only");
        }
        if (options.readOnly && !options.createPath.empty())
        {
            return std::unexpected("--read-only and --create can't be combined: --create writes a project");
        }
        if (options.editorExtensions && !options.playScene.empty())
        {
            return std::unexpected("--editor-extensions and --play can't be combined: a play host never loads editor "
                                   "extensions");
        }
        if (options.editorExtensionsWatch && !options.editorExtensions)
        {
            return std::unexpected("--editor-extensions-watch needs --editor-extensions: it only watches the files that "
                                   "flag loads");
        }
        if (options.createPath.empty() && (givenName || givenId))
        {
            return std::unexpected(std::string(givenName ? "--name" : "--project-id") + " only goes with --create");
        }

        // Fail closed (S3, audit F5): a host that serves on a non-loopback address needs a token and TLS, or an explicit
        // --insecure-bind. --create and --new-token don't serve; an unparsable address counts as non-loopback.
        const bool serves = options.createPath.empty() && options.newTokenUser.empty();
        // Editor extensions run project code on the host for whoever may invoke them, so a host that is reachable from
        // other machines needs roles (a tokens file) for that. --insecure-bind does not lift this.
        if (options.editorExtensions && serves && !IsLoopbackBindAddress(options.bindAddress) && options.tokensFile.empty())
        {
            return std::unexpected(std::format(
                "--editor-extensions needs --tokens-file with --bind {}: that address is not loopback, and editor "
                "extensions run project code on the host, so who may invoke them has to be a role (--insecure-bind does "
                "not lift this)",
                options.bindAddress));
        }
        if (serves && !options.insecureBind && !IsLoopbackBindAddress(options.bindAddress))
        {
            const bool hasToken = !options.tokenEnv.empty() || !options.tokensFile.empty();
            if (!hasToken || !options.tls)
            {
                const char *missing = !hasToken && !options.tls ? "an access token and --tls"
                                      : !hasToken              ? "an access token"
                                                               : "--tls";
                return std::unexpected(std::format(
                    "--bind {} is not a loopback address and the host is missing {}: refusing to start. Add --tls with "
                    "--token-env or --tokens-file (a build without TLS support can't), or pass --insecure-bind to "
                    "accept the risk",
                    options.bindAddress, missing));
            }
        }
        return options;
    }

    void DrainUntilEof(const StdinReader &readSome)
    {
        char buffer[512];
        while (readSome(buffer, sizeof(buffer)) > 0)
        {
        }
    }

    namespace
    {
        std::ptrdiff_t ReadStdinHandle(char *buffer, const std::size_t size)
        {
#ifdef _WIN32
            const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
            {
                return -1; // no stdin at all
            }
            const bool isPipe = GetFileType(handle) == FILE_TYPE_PIPE;
            for (;;)
            {
                DWORD read = 0;
                if (!ReadFile(handle, buffer, static_cast<DWORD>(size), &read, nullptr))
                {
                    return -1; // a broken pipe (the writer closed) is the usual way here
                }
                // A pipe's end of file is the failed read above; a successful read of nothing (a zero-length write by
                // the other side) isn't one. A file or console reports its end as 0 bytes.
                if (read == 0 && isPipe)
                {
                    continue;
                }
                return static_cast<std::ptrdiff_t>(read);
            }
#else
            for (;;)
            {
                const ssize_t read = ::read(0, buffer, size);
                if (read < 0 && errno == EINTR)
                {
                    continue;
                }
                return static_cast<std::ptrdiff_t>(read);
            }
#endif
        }
    }

    std::shared_ptr<std::atomic<bool>> StartStdinEofWatcher()
    {
        // Once per process: stdin is the process's, so a second RunHost shares the first watcher (and its flag, true if
        // stdin has closed) instead of starting a competing reader
        static const std::shared_ptr<std::atomic<bool>> closed = []
        {
            auto flag = std::make_shared<std::atomic<bool>>(false);
            std::thread(
                [flag]
                {
                    DrainUntilEof(ReadStdinHandle);
                    *flag = true;
                })
                .detach();
            return flag;
        }();
        return closed;
    }

    std::string_view HostUsage()
    {
        return R"(N2Engine Editor Host
Usage: N2EditorHost [options]
Options:
  -p, --port <port>         Server port (default: 9999; 0 lets the OS pick a free one)
  --bind <ipv4 address>     Address to listen on (default: 127.0.0.1)
                            Any address outside 127.0.0.0/8 is refused at startup unless the host has a
                            token (--token-env or --tokens-file) and --tls, or --insecure-bind is given
                            (a build without TLS support needs --insecure-bind). A play host (--play) is
                            always plaintext, so it needs --insecure-bind too. Whoever can reach the
                            address (and, without a token, anyone at all) controls this host, including
                            deleting scene files
  --insecure-bind           Accept a non-loopback --bind without a token and --tls, and log the warnings.
                            Unsafe: the access token can be read off the network, or there is none
  --project <path>          Project folder, holding project.n2proj (see --create): res:// assets
                            load from <path>/assets, and its startup scene opens
  --renderer <name>         opengl (default: a hidden window, needs a GPU) or
                            software (no window at all; renders without a GPU or display)
  --token-env <variable>    Read the access token from this environment variable (then removed
                            from the host's environment): every connection must send Hello with it
                            before any other command. One token, one user ("token") with every right;
                            for one token per user and roles, see --tokens-file
  --tokens-file <path>      Per-user tokens: a JSON array of {"user", "role", "hash"} entries (role:
                            admin, editor or viewer; hash: "sha256:" and 64 lowercase hex digits, made by
                            --new-token). Every connection must send Hello with a user's token and gets
                            that user's role: a viewer reads only (and can't play), an editor changes
                            the project but can't Shutdown, an admin can do anything. The file is
                            re-read on each new connection (and on SIGHUP on POSIX), so a user can be
                            revoked without a restart. Needs a host built with N2ENGINE_EDITOR_TLS (for
                            SHA-256); on POSIX the file must not be accessible to group or others
                            (chmod 600). Can't be combined with --token-env
  --new-token <user> <role> Make a token for this user (A-Z a-z 0-9 . _ @ -, up to 64 characters) and
                            role, print it ONCE with the entry to add to the --tokens-file file, and
                            exit. The token is 256 random bits; the file keeps only its hash
  --exit-on-disconnect      Exit once a client's session ends (its connection closes after a
                            successful Hello, or with no token, after it connected)
  --exit-on-stdin-eof       Exit once stdin reaches end of file or can't be read (a launcher's pipe
                            closes when the launcher dies, even before any client connects). Leave it
                            off when stdin may be closed or absent: the host would exit at once
  --tls                     Serve TLS (1.2 or newer) on the editor port. The host makes a self-signed
                            certificate on first use and keeps it, with its key, in the user's data
                            folder (outside any project). The ready line then has tls=1 and the
                            certificate's SHA-256 fingerprint, for the client to pin. Needs a host
                            built with N2ENGINE_EDITOR_TLS. Applies to this process only: a play host
                            (--play) is another process, and stays plaintext
  --read-only               Serve reads only: every command that isn't a read (anything that changes the
                            project, a scene, an asset or the game, and Shutdown) is refused with the
                            readOnly error, and the connection stays open. Hello and the read commands
                            work, with or without --token-env. Commands nobody has classified are refused.
                            Can't be combined with --play (it runs the game and the project's Lua) or
                            --create (it writes a project)
  --editor-extensions       Load the project's Lua editor extensions: the files <project>/editor/**/*.lua
                            (a folder beside assets/, so no protocol write, require or play host reaches
                            it). Each file runs in a sandbox state of its own with a time and memory budget,
                            at start and on ReloadEditorExtensions only, never when a scene loads, and
                            declares menu items with Editor.Extension{...}. A file that fails only marks
                            itself failed. ListEditorExtensions and GetEditorUi list what loaded. OFF by
                            default: it runs project code on this host, so it is a decision of whoever
                            starts it. Can't be combined with --play, nor with a --bind outside 127.0.0.0/8
                            unless --tokens-file is given (--insecure-bind does not lift that). Works with
                            --read-only (the extensions are listed). Without --project there is nothing to load
  --editor-extensions-watch Watch the project's editor/ folder (about once a second: modification time and
                            size, then a content hash) and reload only the extension file that changed, or
                            load one that was added and drop one that was removed; each change tells the
                            clients with extensionsChanged. OFF by default. Needs --editor-extensions
  --no-rate-limit           Turn off the failed-Hello rate limiting. By default, with --token-env or
                            --tokens-file, an address that fails 3 Hellos is backed off (1 s, doubling to
                            5 min; its connections are closed at once, before any TLS handshake), and with
                            --tokens-file a user that fails 3 Hellos is backed off for up to 30 s (a Hello
                            with the right token still gets in). Loopback has no backoff of its own (behind
                            a proxy or an SSH tunnel that connects over loopback everyone is loopback, and
                            a ban would lock out the client with the right token): it is bounded by --preauth-slots and the
                            handshake rate. A connection that never finishes a Hello (idle for the 2 s Hello
                            budget, closed early, a failed TLS handshake or one not done within its own
                            5 s) counts as a failure for its address too, but not for loopback. For test
                            rigs only
  --idle-timeout <seconds>  Close a connection that has sent no request for this long once it is a session
                            (after its Hello with a token; from the start without one). Every request,
                            polled ones included, restarts it. Default: 300 (5 minutes); 0 turns it off.
                            Without it one idle client keeps the host from everyone else
  --write-timeout <seconds> Close a connection when a response makes no write progress for this long (the
                            client isn't reading). Progress restarts it, so a slow reader isn't cut off.
                            Default: 30; at least 1
  --max-session <minutes>   Close a session older than this, whatever it does. Default: off (0)
  --preauth-slots <n>       How many connections may be in their TLS handshake and Hello at once (and how
                            many worker threads serve them), 1 to 16. Default: 4. At most 2 of them for
                            one non-loopback address (loopback may use all: a proxy or an SSH tunnel on
                            the same machine makes every peer loopback; the host sees the address a
                            proxy or container presents). A connection that finds none free is closed
                            at once, before TLS, with no line of its own (one summary line every 10 s).
                            Handshakes are also limited to 20 a second in all and 5 a second per
                            address. The authenticated session has its own thread: a peer that stalls
                            in a slot (a silent connection, a stalled handshake) holds that slot for the
                            handshake and Hello budgets (5 s and 2 s) and nothing else. One session at a
                            time: a second authenticated client of the same user and role takes it over,
                            and a client of a strictly higher role (admin over editor over viewer)
                            preempts it (the old session ends as on a disconnect and the new client
                            waits up to 3 s for that); any other second client waits up to 1 s for the
                            session to end, then gets the error "The host is serving another session; try
                            again later" (naming its role; ErrorCoded busy for a client that asked for
                            error codes). Without a token the newest connection takes over
  --log-level <level>       The least severe level written to stderr (and --log-file): info (the default),
                            warn or error. Every line is "<UTC time> <LEVEL> <message>", one per event,
                            in every build type; the audit, rate-limit and tokens-reload lines are Info or Warn
  --log-file <path>         Also append those lines to this file (created with mode 0600 on POSIX; an
                            existing file keeps its mode). A path that can't be opened ends the host at start.
                            No rotation: logrotate with copytruncate, or restart the host
  --play <file>             Start as a play host: run the scene snapshot in <file> (what the
                            WritePlaySnapshot command writes; or a res:// .scene path in the project)
                            as a game instead of opening the startup scene. Pass the same --project
                            and --renderer as the editing host, and --exit-on-disconnect. Serves
                            SetPaused, Step, SendInput and RenderFrame for the running game
  --create <path>           Make <path> a project (project.n2proj, assets/scenes/Main.scene,
                            assets/scripts/Example.lua), print one line to stdout, and exit
                            without starting the engine. Exit code 0: created; 2: the folder
                            already has a project.n2proj (nothing written); 1: any other error
  --name <name>             With --create: the project's name (default: the folder's name)
  --project-id <uuid>       With --create: the project's id, the namespace of its asset UUIDs
                            (default: a new random one); "from-path" keeps the UUIDs the folder's
                            assets had before projects had ids
  -h, --help                Show this help
Once listening, prints the line "N2EditorHost ready port=<port>" to stdout (with --tls, then
" tls=1 fingerprint=<64 hex digits>").
With --create, prints "N2EditorHost created projectId=<uuid> startupScene=<res path>" instead.
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

    std::string FormatReadyLine(const int port, const std::string_view tlsFingerprint)
    {
        std::string line = std::format("{} port={}", ReadyLinePrefix, port);
        if (!tlsFingerprint.empty())
        {
            line += std::format(" tls=1 fingerprint={}", tlsFingerprint);
        }
        return line;
    }

    std::string FormatCreatedLine(const Math::UUID &projectId, const std::string_view startupScene)
    {
        std::string line = std::format("{} projectId={}", CreatedLinePrefix, projectId.ToString());
        // A value never holds a space (the fields are space-separated); a startup scene with one is left out
        if (!startupScene.empty() && startupScene.find_first_of(" \t\r\n") == std::string_view::npos)
        {
            line += std::format(" startupScene={}", startupScene);
        }
        return line;
    }
}
