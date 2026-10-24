#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/Logger.hpp"
#include "engine/network/n2net/JoinTokens.hpp"

namespace N2Engine::Server
{
    /// Starts the line N2Server prints to stdout once it is listening
    inline constexpr std::string_view ReadyLinePrefix = "N2Server ready";

    /// N2Server's command line, parsed. Kept in the library (not Main.cpp) so it can be unit tested.
    struct ServerOptions
    {
        static constexpr int DefaultPort = 7777;
        static constexpr int DefaultMaxClients = 16;
        /// The most --max-clients takes (UdpTransportOptions::maxPeers is clamped to 4096 and tracks pre-auth sources too)
        static constexpr int MaxMaxClients = 1024;
        /// "N2NT" in ASCII: the protocol id docs/netcode.html uses in its examples. A game gives its own build's id.
        static constexpr std::uint32_t DefaultProtocolId = 0x4E32'4E54;

        /// --project: the project folder (its project.n2proj). Required to serve.
        std::string projectPath;
        /// --scene: a res://<folder>/<name>.scene path inside the project. Empty: the project's startupScene.
        std::string scene;
        /// --bind: a numeric IPv4 or IPv6 address. The default is loopback: nothing listens on every interface unless it
        /// is asked to.
        std::string bindAddress = "127.0.0.1";
        /// --port: 0 lets the OS pick a free port; the ready line reports the one it picked
        int port = DefaultPort;
        /// --tokens-file: the join tokens (n2security's TokenTable: a JSON array of {"user", "role", "hash"}). Without
        /// one nobody can join, so a non-loopback --bind needs it (or --insecure-bind).
        std::string tokensFile;
        /// --max-clients: the most sessions at once (SecureTransportOptions::maxSessions), 1 to MaxMaxClients
        int maxClients = DefaultMaxClients;
        /// --protocol-id: identifies the game and its build; a handshake with another id is dropped. Decimal or 0x hex.
        std::uint32_t protocolId = DefaultProtocolId;
        /// --insecure-bind: accept a non-loopback --bind without a tokens file. Without it ParseServerArguments refuses
        /// such a bind. Every session is encrypted whatever this says; what the flag permits is a server that admits no
        /// one (or, once a tokens file is added later, listens on a world-facing address from the first moment).
        bool insecureBind = false;
        /// --relay: the deterministic relay mode (stage C3, LockstepRelay): the server runs no world, it seats
        /// --relay-players clients and forwards and confirms their inputs. The project must use the Deterministic preset;
        /// otherwise RunServer refuses to start.
        bool relay = false;
        /// --relay-players: players the session waits for, 1 to 4 (the replay format's cap)
        int relayPlayers = 2;
        /// --relay-stall: what the relay does when a slot stalls or its client leaves: "drop" (DropSlot, the session goes
        /// on) or "stop" (end it for everyone)
        bool relayStallStops = false;
        /// --relay-stall-ms: how long the others may wait for a slot before it counts as stalled
        int relayStallMs = 5000;
        /// --relay-layout: a JSON list of input controls (ParseInputLayoutJson); empty: the layout the project's Lua
        /// declared with Network.DeclareInputLayout
        std::string relayLayoutFile;
        /// --websocket-port: -1 disabled, 0 chooses a free loopback TCP port. Put a TLS proxy in front for wss.
        int webSocketPort = -1;
        /// --websocket-origin: exact allowed browser Origin; empty accepts requests with no Origin only.
        std::string webSocketOrigin;
        /// --exit-on-stdin-eof: stop when stdin reaches end of file or fails (a launcher that has gone away)
        bool exitOnStdinEof = false;
        /// --log-level <info|warn|error>
        Logger::LogLevel logLevel = Logger::LogLevel::Info;
        /// --log-file <path>: also append the log lines to this file (created 0600 on POSIX). Empty: stderr only.
        std::string logFile;
        /// --new-token <user> <role>: make a join token for this user, add its hash to --tokens-file, print the token
        /// once and exit; the server isn't started. Empty user: not asked for.
        std::string newTokenUser;
        N2Net::JoinRole newTokenRole = N2Net::JoinRole::Player;
        bool showHelp = false;
    };

    /// A decimal number in [0, 4294967295], or "0x" and 1 to 8 hexadecimal digits, nothing else (no sign, whitespace or
    /// trailing text)
    [[nodiscard]] std::optional<std::uint32_t> ParseProtocolId(std::string_view text);

    /// The arguments after the program name. -h/--help anywhere wins: the result is the defaults with showHelp set,
    /// whatever else is there. Otherwise an option missing its value (none follows, or the next argument starts with
    /// "--"), an invalid value, an unknown option or an argument that is not an option at all is an error (the message
    /// names it), unlike N2EditorHost, which ignores unknown arguments: a typo must never leave a server running with
    /// settings the operator thought they had changed. A server that would serve (not --new-token or --help) needs
    /// --project, and with a non-loopback --bind (N2Net::IsLoopbackAddressText) needs --tokens-file unless
    /// --insecure-bind is given. --new-token needs --tokens-file.
    [[nodiscard]] std::expected<ServerOptions, std::string> ParseServerArguments(const std::vector<std::string> &args);

    /// N2Server's usage text (what --help prints), one option per line
    [[nodiscard]] std::string_view ServerUsage();

    /// The ready line, without the newline: "N2Server ready port=<port> bind=<address>". A launcher matches a whole
    /// line starting with ReadyLinePrefix followed by space-separated key=value fields (values never hold a space);
    /// port is always present and later fields may be appended, so a parser must ignore keys it doesn't know.
    [[nodiscard]] std::string FormatReadyLine(int port, std::string_view bindAddress);
}
