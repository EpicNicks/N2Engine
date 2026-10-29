#include "n2server/ServerOptions.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <system_error>

#include "editor-server/HostOptions.hpp"
#include "editor-server/LogSink.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/security/TokenTable.hpp"

namespace N2Engine::Server
{
    namespace
    {
        constexpr std::string_view kUsage =
            "N2Server: the headless dedicated game server (no window, no renderer, no audio)\n"
            "\n"
            "Usage: N2Server --project <folder> [options]\n"
            "       N2Server --new-token <user> <role> --tokens-file <path>\n"
            "\n"
            "  --project <folder>      the project folder (its project.n2proj); required\n"
            "  --scene <res://...>     the scene to run, a res://<folder>/<name>.scene path (default: the project's startup scene)\n"
            "  --bind <address>        a numeric IPv4 or IPv6 address (default 127.0.0.1; 0.0.0.0 or :: listens on every interface)\n"
            "  -p, --port <n>          the UDP port, 0 for any free port (default 7777); the ready line reports it\n"
            "  --tokens-file <path>    the join tokens (a JSON array of {\"user\", \"role\", \"hash\"}); without one nobody can join\n"
            "  --websocket-port <n>   enable loopback WebSocket listener, 0 for a free port; use a TLS proxy for public wss\n"
            "  --websocket-origin <o> exact allowed browser Origin (default: non-browser clients only)\n"
            "  --max-clients <n>       the most sessions at once, 1 to 1024 (default 16)\n"
            "  --protocol-id <n>       identifies the game and its build, decimal or 0x hex (default 0x4E324E54)\n"
            "  --insecure-bind         accept a non-loopback --bind without a tokens file\n"
            "  --relay                 the deterministic relay mode: seat --relay-players clients, forward and confirm their\n"
            "                          inputs, stop on a desync (the project must use network.preset deterministic)\n"
            "  --relay-players <n>     players the relay waits for, 1 to 4 (default 2)\n"
            "  --relay-stall <p>       drop (default: DropSlot, the session goes on) or stop, for a stalled or lost client\n"
            "  --relay-stall-ms <n>    how long the others wait for a slot before it is stalled, 100 to 600000 (default 5000)\n"
            "  --relay-layout <path>   a JSON list of input controls; default: the layout the project's Lua declared\n"
            "  --relay-spectators <n>  spectators the relay takes at once, 0 to 16, 0 refuses every one (default 4)\n"
            "  --relay-snapshot-max-mb <n>\n"
            "                          the largest snapshot the relay passes to a spectator, in MiB, 1 to 64 (default 16)\n"
            "  --relay-snapshot-timeout-ms <n>\n"
            "                          how long a spectator waits for its snapshot to begin, 1000 to 120000 (default 10000)\n"
            "  --exit-on-stdin-eof     stop when stdin closes (a launcher that has gone away)\n"
            "  --log-level <level>     info (default), warn or error\n"
            "  --log-file <path>       also append the log lines to this file (created 0600 on POSIX)\n"
            "  --new-token <user> <role>\n"
            "                          make a join token (role player, spectator or admin), add its SHA-256 to --tokens-file,\n"
            "                          print the token once and exit; the server is not started\n"
            "  -h, --help              this text\n";

        /// Option text as it may be repeated in an error: control characters written out
        std::string Shown(const std::string_view text)
        {
            return Editor::EscapeLogText(text);
        }
    }

    std::optional<std::uint32_t> ParseProtocolId(const std::string_view text)
    {
        int base = 10;
        std::string_view digits = text;
        if (text.starts_with("0x"))
        {
            base = 16;
            digits = text.substr(2);
        }
        if (digits.empty())
        {
            return std::nullopt;
        }
        for (const char c : digits)
        {
            const bool decimal = c >= '0' && c <= '9';
            const bool hex = base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
            if (!decimal && !hex)
            {
                return std::nullopt;
            }
        }
        std::uint32_t value = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (error != std::errc{} || end != digits.data() + digits.size())
        {
            return std::nullopt;
        }
        return value;
    }

    std::string_view ServerUsage()
    {
        return kUsage;
    }

    std::string FormatReadyLine(const int port, const std::string_view bindAddress)
    {
        return std::format("{} port={} bind={}", ReadyLinePrefix, port, bindAddress);
    }

    std::expected<ServerOptions, std::string> ParseServerArguments(const std::vector<std::string> &args)
    {
        ServerOptions options;

        // Help wins: "N2Server --port x --help" shows the usage instead of complaining about the port
        for (const std::string &arg : args)
        {
            if (arg == "-h" || arg == "--help")
            {
                options.showHelp = true;
                return options;
            }
        }

        for (std::size_t i = 0; i < args.size(); ++i)
        {
            const std::string &arg = args[i];
            if (arg == "--insecure-bind")
            {
                options.insecureBind = true;
                continue;
            }
            if (arg == "--relay")
            {
                options.relay = true;
                continue;
            }
            if (arg == "--exit-on-stdin-eof")
            {
                options.exitOnStdinEof = true;
                continue;
            }
            if (arg == "--new-token")
            {
                // Two values: "--new-token alice player". A value starting with "--" is the next option.
                if (i + 2 >= args.size() || args[i + 1].starts_with("--") || args[i + 2].starts_with("--"))
                {
                    return std::unexpected("--new-token needs a user and a role (admin, player or spectator)");
                }
                const std::string &user = args[++i];
                const std::string &roleText = args[++i];
                if (!Security::IsValidTokenUserName(user))
                {
                    return std::unexpected(std::format("--new-token: the user must be 1 to {} characters of "
                                                       "A-Z a-z 0-9 . _ @ -",
                                                       Security::MaxTokenUserLength));
                }
                const std::optional<N2Net::JoinRole> role = N2Net::ParseJoinRole(roleText);
                if (!role)
                {
                    return std::unexpected("--new-token: the role must be admin, player or spectator");
                }
                options.newTokenUser = user;
                options.newTokenRole = *role;
                continue;
            }
            const bool takesValue = arg == "-p" || arg == "--port" || arg == "--bind" || arg == "--project" ||
                                    arg == "--scene" || arg == "--tokens-file" || arg == "--max-clients" ||
                                    arg == "--websocket-port" || arg == "--websocket-origin" || arg == "--protocol-id" || arg == "--log-level" || arg == "--log-file" ||
                                    arg == "--relay-players" || arg == "--relay-stall" || arg == "--relay-stall-ms" || arg == "--relay-layout" ||
                                    arg == "--relay-spectators" || arg == "--relay-snapshot-max-mb" || arg == "--relay-snapshot-timeout-ms";
            if (!takesValue)
            {
                return std::unexpected(std::format("Unknown option: {} (see --help)", Shown(arg)));
            }
            // "--project --port 0" is a forgotten path, not a project folder called "--port"
            if (i + 1 >= args.size() || args[i + 1].starts_with("--"))
            {
                return std::unexpected(arg + " is missing a value");
            }
            const std::string &value = args[++i];

            if (arg == "--relay-players")
            {
                const std::optional<int> players = Editor::ParseWholeNumber(value, 1, 4);
                if (!players)
                {
                    return std::unexpected("Invalid --relay-players: " + Shown(value) + " (expected a whole number from 1 to 4)");
                }
                options.relayPlayers = *players;
                continue;
            }
            if (arg == "--relay-stall")
            {
                if (value != "drop" && value != "stop")
                {
                    return std::unexpected("Invalid --relay-stall: " + Shown(value) + " (expected drop or stop)");
                }
                options.relayStallStops = value == "stop";
                continue;
            }
            if (arg == "--relay-stall-ms")
            {
                const std::optional<int> ms = Editor::ParseWholeNumber(value, 100, 600000);
                if (!ms)
                {
                    return std::unexpected("Invalid --relay-stall-ms: " + Shown(value) + " (expected a whole number from 100 to 600000)");
                }
                options.relayStallMs = *ms;
                continue;
            }
            if (arg == "--relay-spectators")
            {
                const std::optional<int> count = Editor::ParseWholeNumber(value, 0, 16);
                if (!count)
                {
                    return std::unexpected("Invalid --relay-spectators: " + Shown(value) + " (expected a whole number from 0 to 16)");
                }
                options.relaySpectators = *count;
                continue;
            }
            if (arg == "--relay-snapshot-max-mb")
            {
                const std::optional<int> mb = Editor::ParseWholeNumber(value, 1, 64);
                if (!mb)
                {
                    return std::unexpected("Invalid --relay-snapshot-max-mb: " + Shown(value) + " (expected a whole number from 1 to 64)");
                }
                options.relaySnapshotMaxMb = *mb;
                continue;
            }
            if (arg == "--relay-snapshot-timeout-ms")
            {
                const std::optional<int> ms = Editor::ParseWholeNumber(value, 1000, 120000);
                if (!ms)
                {
                    return std::unexpected("Invalid --relay-snapshot-timeout-ms: " + Shown(value) +
                                           " (expected a whole number from 1000 to 120000)");
                }
                options.relaySnapshotTimeoutMs = *ms;
                continue;
            }
            if (arg == "--relay-layout")
            {
                if (value.empty())
                {
                    return std::unexpected("--relay-layout is missing a value");
                }
                options.relayLayoutFile = value;
                continue;
            }
            if (arg == "--websocket-origin")
            {
                if (value.size() > 2048 || value.find_first_of("\r\n") != std::string::npos)
                    return std::unexpected("Invalid WebSocket Origin");
                options.webSocketOrigin = value;
                continue;
            }
            if (arg == "--websocket-port")
            {
                const auto port = Editor::ParsePort(value);
                if (!port) return std::unexpected("Invalid WebSocket port");
                options.webSocketPort = *port;
                continue;
            }
            if (arg == "-p" || arg == "--port")
            {
                const std::optional<int> port = Editor::ParsePort(value);
                if (!port)
                {
                    return std::unexpected("Invalid port: " + Shown(value));
                }
                options.port = *port;
            }
            else if (arg == "--bind")
            {
                if (value.empty())
                {
                    return std::unexpected("--bind is missing a value");
                }
                options.bindAddress = value;
            }
            else if (arg == "--project")
            {
                if (value.empty())
                {
                    return std::unexpected("--project is missing a value");
                }
                options.projectPath = value;
            }
            else if (arg == "--scene")
            {
                if (!IO::ProjectFile::IsScenePath(value))
                {
                    return std::unexpected("Invalid --scene: " + Shown(value) +
                                           " (expected res://<folder>/<name>.scene)");
                }
                options.scene = value;
            }
            else if (arg == "--tokens-file")
            {
                // An empty path would otherwise mean "no tokens file" and leave the server open to nobody, or to a
                // bind it would have refused
                if (value.empty())
                {
                    return std::unexpected("--tokens-file is missing a value");
                }
                options.tokensFile = value;
            }
            else if (arg == "--max-clients")
            {
                const std::optional<int> clients = Editor::ParseWholeNumber(value, 1, ServerOptions::MaxMaxClients);
                if (!clients)
                {
                    return std::unexpected(std::format("Invalid --max-clients: {} (expected a whole number from 1 to {})",
                                                       Shown(value), ServerOptions::MaxMaxClients));
                }
                options.maxClients = *clients;
            }
            else if (arg == "--protocol-id")
            {
                const std::optional<std::uint32_t> id = ParseProtocolId(value);
                if (!id)
                {
                    return std::unexpected("Invalid --protocol-id: " + Shown(value) +
                                           " (expected a decimal number or 0x and hexadecimal digits, at most 32 bits)");
                }
                options.protocolId = *id;
            }
            else if (arg == "--log-level")
            {
                const std::optional<Logger::LogLevel> level = Editor::ParseLogLevel(value);
                if (!level)
                {
                    return std::unexpected("Invalid --log-level: " + Shown(value) + " (expected info, warn or error)");
                }
                options.logLevel = *level;
            }
            else if (arg == "--log-file")
            {
                if (value.empty())
                {
                    return std::unexpected("--log-file is missing a value");
                }
                options.logFile = value;
            }
        }

        if (!options.newTokenUser.empty())
        {
            if (options.tokensFile.empty())
            {
                return std::unexpected("--new-token needs --tokens-file: the file the token's hash is added to");
            }
            return options;
        }

        if (options.projectPath.empty())
        {
            return std::unexpected("--project is required (see --help)");
        }
        if (!options.relay && (options.relayPlayers != 2 || options.relayStallStops || options.relayStallMs != 5000 ||
                               !options.relayLayoutFile.empty() || options.relaySpectators != 4 ||
                               options.relaySnapshotMaxMb != 16 || options.relaySnapshotTimeoutMs != 10000))
        {
            return std::unexpected("--relay-players, --relay-stall, --relay-stall-ms, --relay-layout, --relay-spectators, "
                                   "--relay-snapshot-max-mb and --relay-snapshot-timeout-ms only apply with --relay");
        }
        // Fail closed: a server on a non-loopback address must have a tokens file (every session is encrypted, but a
        // server with no tokens admits nobody), or be told explicitly that it may
        if (!N2Net::IsLoopbackAddressText(options.bindAddress) && options.tokensFile.empty() && !options.insecureBind)
        {
            return std::unexpected(std::format(
                "Refusing to bind {}: it is not a loopback address and there is no --tokens-file, so nobody could "
                "join. Add --tokens-file <path> (make tokens with --new-token), or --insecure-bind to start anyway.",
                Shown(options.bindAddress)));
        }
        return options;
    }
}
