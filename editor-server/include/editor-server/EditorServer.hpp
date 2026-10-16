#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <cstdint>
#include <string>
#include <string_view>

#include <math/Vector2.hpp>

#include "engine/input/KeySource.hpp"
#include "engine/io/ProjectFile.hpp"
#include "engine/io/ResourceLoader.hpp"
#include "engine/io/ResourcePath.hpp"

#include "editor-server/CommandQueue.hpp"
#include "editor-server/Connection.hpp"
#include "editor-server/EditHistory.hpp"
#include "editor-server/EditorCamera.hpp"
#include "editor-server/EventRing.hpp"
#include "editor-server/FrameTracker.hpp"
#include "editor-server/LogCoalescer.hpp"
#include "editor-server/PreAuthSlots.hpp"
#include "editor-server/RateLimiter.hpp"
#include "editor-server/SessionArbiter.hpp"
#include "editor-server/TokenBucket.hpp"
#include "editor-server/TokenTable.hpp"

namespace Renderer::Common
{
    class IRenderer;
}

namespace N2Engine
{
    class Scene;
}

namespace N2Engine::Editor
{
    class EditorTlsServer;
    class EditorTlsSession;
}

namespace N2Engine::Editor::Protocol
{
    struct HelloCmd;
}

namespace N2Engine::Editor
{

    /// The open scene, as OpenScene, NewScene, SaveSceneToFile and GetOpenScene report it (SceneInfo)
    struct OpenSceneInfo
    {
        /// Its file, as a res:// path; empty when it has none (a new scene not saved yet, or one LoadScene sent)
        std::string path;
        std::string name;
        /// The scene's UUID: its file's asset UUID (ResourceUUID::FromPath) when it has a file
        std::string uuid;
        /// The scene revision (see EditorServer::GetSceneRevision)
        uint32_t revision = 0;
        /// The revision when the scene was last opened from or saved to its file; the scene has unsaved changes
        /// exactly when revision != savedRevision
        uint32_t savedRevision = 0;
    };

    /// A scene path a client sent, resolved: the res:// path (normalised) and the file it names
    struct ResolvedScenePath
    {
        IO::ResourcePath resourcePath;
        std::filesystem::path file;
    };

    /// TCP host for the editor client.
    ///
    /// Threading model: network threads never touch engine state (scenes, Lua, GLFW/GL, the Logger). There are
    ///  - one ACCEPTOR thread (_serverThread): accepts, reads the peer address, applies the failed-Hello backoff, the
    ///    pre-auth slots and the handshake rate limit, and hands the socket to a worker; it also re-reads the tokens file;
    ///  - N PRE-AUTH WORKERS (SetPreAuthSlots, default 4): each takes one connection at a time through its TLS handshake
    ///    and, with an access token, its one Hello frame (verified there: token and protocol major version), inside the
    ///    handshake and Hello budgets; a peer that stalls here holds one worker for at most those budgets and nobody else;
    ///  - ONE SESSION thread: serves the one authenticated connection at a time (the request loop with the idle, write
    ///    and age deadlines).
    /// The main thread owns the engine. Each request is enqueued on a CommandQueue and the session thread blocks until the
    /// main thread has run it: the host's main loop calls ProcessCommands() every iteration, which executes the queued
    /// commands (handlers, rendering and logging all happen there) and hands each response frame back for the session
    /// thread to send. Shutdown is the one command answered on the session thread, since it only stops the server.
    ///
    /// Sessions: a client opens one with Hello (protocol version, access token), and it lasts as long as the
    /// connection, so a client that reconnects sends Hello again. Hello is optional unless the server has an access
    /// token (SetAccessToken): then a connection that hasn't sent a good Hello gets nothing but its refusal. A worker
    /// that verified a Hello offers the connection to the session thread, which REPLAYS that Hello through the ordinary
    /// Hello command on the main thread (it authenticates again, binds the identity and builds ServerInfo exactly as
    /// before, and refuses if the token was revoked in the meantime). While a session is active another authenticated
    /// client waits for it for BusyGrace (a session that is just ending), then gets a plain Error (BusyError) and is
    /// closed; a host without an access token keeps serving one connection at a time and a second one waits. A
    /// connection that hasn't succeeded a Hello can't hold more than a worker for its budgets: it is closed after a
    /// refused command or a failed Hello, and when no Hello has succeeded within the Hello timeout.
    ///
    /// Events: the server keeps an EventRing that PollEvents reads. From construction to destruction it is subscribed to
    /// the Logger, so every line logged in the process (the server's own included) becomes a log event; a host that
    /// constructs the server before Application::Init keeps the engine's startup lines for a client that connects later.
    /// Nothing the ring or PollEvents does logs, so polling an idle server finds no events.
    ///
    /// Security: the server binds to loopback by default. Without an access token there is no authentication, so
    /// any local process can drive the host (including deleting scene files); with --bind, anything that can reach it.
    class EditorServer
    {
    public:
        static constexpr const char *DefaultBindAddress = "127.0.0.1";
        /// Requests declaring a larger payload are refused (and the connection closed) before allocating
        static constexpr uint32_t MaxPayloadBytes = 64u * 1024u * 1024u;
        /// The same, on a connection to a server with an access token that hasn't yet succeeded a Hello (far more
        /// than a Hello's three strings need), so an unauthorised client can't make the server allocate much
        static constexpr uint32_t MaxPayloadBytesBeforeHello = 64u * 1024u;
        /// Largest accepted viewport width/height; the frame buffer is width * height * 4 bytes
        static constexpr int32_t MaxViewportDimension = 4096;
        /// The most entityIds a sceneChanged event carries; a change that touched more carries none, so one event
        /// stays small however large the subtree that was deleted or duplicated
        static constexpr size_t MaxEventEntityIds = 256;
        /// DeleteScene only removes files with this extension (any case) inside the scenes directory
        static constexpr const char *SceneFileExtension = ".json";

        /// Subscribes the event ring to the Logger (see Events above)
        EditorServer();
        /// Stops the server, then unsubscribes the event ring
        ~EditorServer();

        EditorServer(const EditorServer &) = delete;
        EditorServer& operator=(const EditorServer &) = delete;

        /// Main thread. False if the socket couldn't be set up (the server isn't running then).
        bool Start(int port, const std::string &bindAddress = DefaultBindAddress);
        /// Main thread. Disconnects any client and joins the acceptor, every pre-auth worker and the session thread (each
        /// polls for stop between waits of at most 100 ms, so this returns within about that, plus the time a command the
        /// session is waiting on takes to be abandoned: closing the queue wakes it). No thread closes a socket another
        /// thread waits on: each connection is closed by the thread that owns it as it ends, and what is left (a
        /// connection still queued for a worker or waiting in the hand-off, the listening socket) is closed here after
        /// the joins.
        void Stop();
        [[nodiscard]] bool IsRunning() const { return _running; }
        /// The port listened on (useful after Start(0)); 0 when not started
        [[nodiscard]] int GetPort() const { return _port; }

        /// Main thread: runs every queued request, waiting up to maxWait for one to arrive.
        size_t ProcessCommands(std::chrono::milliseconds maxWait = std::chrono::milliseconds::zero());

        /// Main thread, once per main-loop iteration: paces the headless engine's loopback audio by the real
        /// time since the previous call (AudioSystem::AdvanceStream, then Update to recycle finished one-shots).
        /// Runs whether or not a client is connected, so sounds finish; the stream buffer it fills is bounded,
        /// and GetAudio drains it. No-op unless audio was initialized with a loopback device.
        void UpdateAudio();
        // (A play host's audio is paced by its game frames, Application::Tick, so UpdateAudio does nothing there.)

        /// Main thread: runs one request and returns its response frame. Never throws: a failing
        /// request (malformed payload, engine exception) produces an Error response.
        std::vector<uint8_t> ExecuteCommand(uint8_t commandType, const std::vector<uint8_t> &payload);

        /// Before Start (the network thread reads it): a non-empty token makes every connection send Hello with this
        /// token before any other command. Empty (the default) leaves Hello optional and accepts any token in it.
        /// False, changing nothing, while the server is running.
        /// Not with a tokens file (SetTokensFile): the two are exclusive, so this then returns false and changes nothing.
        bool SetAccessToken(std::string token);
        /// Whether Hello must carry a token: the single access token, or a tokens file
        [[nodiscard]] bool RequiresAccessToken() const { return !_accessToken.empty() || !_tokensFile.empty(); }

        // ==================== Per-user tokens ====================

        /**
         * Before Start (the network thread uses it): per-user tokens from a JSON file (see TokenTable): a connection must
         * Hello with the token of one of its users and gets that user's role (admin, editor or viewer) for as long as it
         * lasts. Loads the file now. An error, changing nothing, when this build has no TLS library (the tokens are
         * SHA-256 hashes: N2ENGINE_EDITOR_TLS), when SetAccessToken has been used (they are exclusive), while running,
         * or when the file can't be loaded (LoadTokensFile: missing, readable by group or others on POSIX, malformed).
         *
         * The file is checked again, on the network thread, whenever a connection is accepted: if its size or
         * modification time changed it is reloaded, and so it is when RequestTokensReload is called (the host does so on
         * SIGHUP). A reload that fails keeps the previous tokens and logs a warning. A reload that removes the user of
         * the open connection, or changes its role or token, closes that connection.
         */
        [[nodiscard]] std::expected<void, std::string> SetTokensFile(const std::filesystem::path &path);
        [[nodiscard]] bool HasTokensFile() const { return !_tokensFile.empty(); }
        /// Any thread, and safe in a signal handler (it only sets a flag): the network thread re-reads the tokens file
        /// (whatever its signature) within about 100 ms
        void RequestTokensReload() { _tokensReloadRequested.store(true); }
        /// Whether a role may run a command: an admin any, an editor any but those of the Admin class, a viewer only
        /// the Read class. Default deny: a command nobody classified is only for an admin.
        [[nodiscard]] static bool RoleAllows(TokenRole role, uint8_t commandType);
        /// The refusal of a role: "forbidden: role <role> is not allowed to run <command>"
        [[nodiscard]] static std::string RoleDeniedMessage(TokenRole role, uint8_t commandType);

        /**
         * Before Start (the network thread uses it): serves TLS 1.2 or newer on the editor port instead of plaintext
         * (--tls). Loads the self-signed certificate and key in `directory`, making them there on first use (see
         * EditorTlsServer::Load). A TLS handshake runs right after each accept and must finish inside the Hello
         * timeout, whether or not there is an access token; a connection whose handshake fails or stalls is closed and
         * the server keeps accepting. The Hello and access-token rules and the pre-Hello payload cap are unchanged, and
         * apply over TLS. An error (changing nothing) when TLS isn't built in (N2ENGINE_EDITOR_TLS), while running, or
         * when the identity can't be loaded.
         */
        [[nodiscard]] std::expected<void, std::string> EnableTls(const std::filesystem::path &directory);
        [[nodiscard]] bool IsTlsEnabled() const { return _tlsServer != nullptr; }
        /// The certificate's SHA-256 as 64 lowercase hex digits (what a client pins); empty without TLS
        [[nodiscard]] std::string TlsFingerprint() const;

        /// The engine version Hello reports (CMake's project version)
        [[nodiscard]] static std::string_view EngineVersion();
        /// The optional features Hello reports, beyond what the protocol version implies. None are defined yet.
        [[nodiscard]] static std::vector<std::string> Capabilities();
        /// Whether a token matches the access token, comparing every byte (the time taken doesn't depend on where
        /// they first differ, so it doesn't reveal how much of a guess was right)
        [[nodiscard]] static bool TokensMatch(std::string_view accessToken, std::string_view token);
        /// The only command a server with an access token answers before a successful Hello: Hello itself
        [[nodiscard]] static bool IsAllowedBeforeHello(uint8_t commandType);
        /// The Error a server with an access token answers any other command with before a successful Hello
        static constexpr const char *HelloRequiredError =
            "Not authorized: this host requires Hello with its access token before any other command";
        /// How long a connection has, with an access token, to send a successful Hello: from being accepted without TLS,
        /// from the end of its TLS handshake with it (the handshake has its own budget, below). 2 s: the server serves
        /// one connection at a time, so this is the longest a peer that says nothing can hold it after connecting (a
        /// host with a token also counts each such connection as a failure for the peer's address, see Failed-Hello
        /// rate limiting).
        static constexpr std::chrono::milliseconds DefaultHelloTimeout{2000};
        /// Before Start, like SetAccessToken; false, changing nothing, while running (or for a non-positive timeout)
        bool SetHelloTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] std::chrono::milliseconds GetHelloTimeout() const { return _helloTimeout; }
        /// How long a connection has, from being accepted, to finish its TLS handshake (TLS hosts only, with or without
        /// an access token). 5 s, what the handshake had before the Hello budget was cut to 2 s: an unoptimised (Debug)
        /// build's handshake can take over 2 s on a slow machine for a legitimate client. A handshake that fails or runs
        /// out of time is a failed Hello for the peer's address, like a Hello that never comes. A peer can so hold the
        /// server for up to this plus the Hello budget (7 s by default) per connection.
        static constexpr std::chrono::milliseconds DefaultTlsHandshakeTimeout{5000};
        /// Before Start, like SetHelloTimeout; false, changing nothing, while running (or for a non-positive timeout)
        bool SetTlsHandshakeTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] std::chrono::milliseconds GetTlsHandshakeTimeout() const { return _tlsHandshakeTimeout; }

        // ==================== Connection deadlines ====================

        /**
         * What keeps one connection from holding the (one-at-a-time) server for ever. Besides the pre-Hello budget above:
         *  - idle timeout: once the connection is a session (at once without an access token, at its first good Hello with
         *    one), it is closed when no complete request has arrived for this long. Every request resets it, polled ones
         *    included, so a client that polls (the editor does) is never idle. Zero turns it off.
         *  - write timeout: a response that makes no progress being written (the client isn't reading) for this long
         *    closes the connection. Progress restarts it, so a slow client that does read a large frame isn't cut off.
         *  - maximum session age: a session older than this is closed, whatever it does. Zero (the default) is off.
         * Each close logs one line naming the reason and the peer. Setters are before Start, like SetHelloTimeout.
         */
        static constexpr std::chrono::milliseconds DefaultIdleTimeout{5 * 60 * 1000};
        static constexpr std::chrono::milliseconds DefaultWriteTimeout{30 * 1000};
        /// Zero turns the idle deadline off; false, changing nothing, while running or for a negative timeout
        bool SetIdleTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] std::chrono::milliseconds GetIdleTimeout() const { return _idleTimeout; }
        /// Positive only (a write with no deadline is what this exists to prevent); false, changing nothing, otherwise
        bool SetWriteTimeout(std::chrono::milliseconds timeout);
        [[nodiscard]] std::chrono::milliseconds GetWriteTimeout() const { return _writeTimeout; }
        /// Zero (off) by default; false, changing nothing, while running or for a negative age
        bool SetMaxSessionAge(std::chrono::milliseconds age);
        [[nodiscard]] std::chrono::milliseconds GetMaxSessionAge() const { return _maxSessionAge; }

        /// The time every connection deadline above (and the TLS handshake and Hello ones) reads: the real steady clock when unset, so a
        /// test can move it without sleeping. The rate limiter's clock (SetRateLimitClock) is separate. A deadline is
        /// noticed within one poll interval (0.1 s of real time) of the clock passing it.
        using ConnectionClock = std::function<std::chrono::steady_clock::time_point()>;
        bool SetConnectionClock(ConnectionClock clock);
        /// Called on the network thread with each accepted socket, before anything is read from it; for tests (a small
        /// send buffer, say). Not for production use.
        using AcceptedSocketHook = std::function<void(int socket)>;
        bool SetAcceptedSocketHook(AcceptedSocketHook hook);

        // ==================== Pre-auth slots, handshake rate and the one session ====================

        /**
         * What keeps connections that have not authenticated from costing the host more than a fixed amount of threads,
         * memory, CPU and time (audit F1, F2, F6; design: security-design Part 2). Every setter is before Start, like
         * SetAccessToken: false, changing nothing, while running.
         *
         *  - SetPreAuthSlots(n): n connections at once may be in their TLS handshake and Hello (n workers; 1-16, default
         *    4). With the Hello timeout this is the longest a silent peer holds anything, and it holds one slot, not the
         *    host. When every slot is taken a new connection is closed at once, with no TLS and no log line of its own
         *    (counted, and reported in one summary line every 10 s).
         *  - SetPreAuthSlotsPerAddress(n): at most n slots for one NON-loopback address (default 2). Loopback may use them
         *    all: behind a reverse proxy or an SSH tunnel every peer is loopback, so no address can be told apart.
         *  - SetHandshakeRateLimit: a token bucket per host and per non-loopback address on connections that would start
         *    a handshake; the excess is closed before TLS. Off in the class (nullopt) so a test that makes many
         *    connections from one pretend address is not throttled; N2EditorHost turns it on (20/s in all, 5/s per
         *    address). The clock is the real steady clock unless one is given.
         *  - the second authenticated client while a session is active (PR S-4; SessionArbiter decides, the rule is in its
         *    header): a client of the SAME user and role takes the session over, a client of a STRICTLY HIGHER role
         *    (admin > editor > viewer) preempts it, anybody else is turned away with Busy. Takeover and preemption end the
         *    active session the way a disconnect does (its open edit groups are closed, its autosave flushed, before the
         *    new Hello runs); the new client waits for that in its slot for SetTakeoverWait (default 3 s) and gets Busy if
         *    the old session has not ended by then. A client that would be turned away waits SetBusyGrace (default 1 s)
         *    first, since a session whose client just went away is still ending for a moment. Busy is ErrorCoded `busy`
         *    (protocol 1.13.0) for a client whose Hello asked for errorCodes and a plain Error otherwise; the text names
         *    the role of the session it is turned away from, never its user.
         *  - a host without an access token has no Hello and every client is an admin with no name, so the NEWEST
         *    connection takes over (before PR S-4 the second one waited in the backlog for as long as the first lasted).
         *  - TCP keepalive on the session socket (Connection::EnableKeepAlive), so a half-open session is noticed by the
         *    kernel.
         */
        static constexpr size_t DefaultPreAuthSlots = 4;
        static constexpr size_t MaxPreAuthSlots = 16;
        static constexpr size_t DefaultPreAuthSlotsPerAddress = 2;
        static constexpr std::chrono::milliseconds DefaultBusyGrace{1000};
        /// How long a client that takes a session over (same user, or a higher role) waits for the old one to end
        static constexpr std::chrono::milliseconds DefaultTakeoverWait{3000};
        /// What a client turned away is answered with when the session it was turned away from is not known (a plain
        /// Error, or the message of ErrorCoded `busy`); BusyMessage adds the holder's role
        static constexpr const char *BusyError = "The host is serving another session; try again later";
        /// BusyError naming the role of the session that holds the host ("... (a viewer session) ..."), never its user
        [[nodiscard]] static std::string BusyMessage(std::optional<TokenRole> holder);
        /// A value outside 1-16 is refused (false)
        bool SetPreAuthSlots(size_t slots);
        [[nodiscard]] size_t GetPreAuthSlots() const { return _preAuthSlotOptions.totalSlots; }
        bool SetPreAuthSlotsPerAddress(size_t slots);
        [[nodiscard]] size_t GetPreAuthSlotsPerAddress() const { return _preAuthSlotOptions.perAddressSlots; }
        bool SetHandshakeRateLimit(std::optional<TokenBucketOptions> options, TokenBucket::Clock clock = {});
        [[nodiscard]] bool HasHandshakeRateLimit() const { return _handshakeRate.has_value(); }
        bool SetBusyGrace(std::chrono::milliseconds grace);
        /// Zero or more: how long a taking-over client waits (0: it takes over only a session that has already ended)
        bool SetTakeoverWait(std::chrono::milliseconds wait);
        [[nodiscard]] std::chrono::milliseconds GetTakeoverWait() const { return _takeoverWait; }
        /// Called on the session thread once the session's socket is set up (keepalive on), before its first request. For
        /// tests (a socket option can be read back here). Not for production use; before Start.
        using SessionStartHook = std::function<void(int socket)>;
        bool SetSessionStartHook(SessionStartHook hook);
        /// Called on the session thread when its connection has ended, before its cleanup runs and before the session is
        /// marked ended: a hook that blocks is a session that cannot end (a stuck write). For tests. Not for production
        /// use; before Start. A hook that blocks must be released before Stop.
        using SessionEndHook = std::function<void()>;
        bool SetSessionEndHook(SessionEndHook hook);
        /// Sessions closed for a newer connection of the same user (and role), or of a host with no token (any thread)
        [[nodiscard]] uint64_t SessionTakeovers() const { return _sessionTakeovers.load(); }
        /// Sessions closed for a client of a strictly higher role (any thread)
        [[nodiscard]] uint64_t SessionPreemptions() const { return _sessionPreemptions.load(); }
        /// How often the acceptor reports the connections it closed without a word (slots taken, handshake rate spent): at
        /// most one line per interval, and none when there was nothing to report. Default 10 s (real time).
        static constexpr std::chrono::milliseconds DefaultAdmissionSummaryInterval{10000};
        bool SetAdmissionSummaryInterval(std::chrono::milliseconds interval);
        /// Called on a pre-auth worker with a verified Hello, just before the connection is offered to the session (for
        /// tests: to change the world between the worker's check and the replay). Not for production use.
        using PreHandoffHook = std::function<void()>;
        bool SetPreHandoffHook(PreHandoffHook hook);
        /// Connections closed at accept because no pre-auth slot was free for them, or the handshake rate was spent (any
        /// thread), since the server was built
        [[nodiscard]] uint64_t PreAuthRefusedConnections() const { return _preAuthRefusedConnections.load(); }
        /// Connections turned away with Busy (any thread), since the server was built
        [[nodiscard]] uint64_t BusyRefusals() const { return _busyRefusals.load(); }
        /// Connections handed to the session thread (any thread), since the server was built; for tests
        [[nodiscard]] uint64_t SessionHandoffs() const { return _sessionHandoffs.load(); }
        /// Slots in use right now (any thread); 0 when the server isn't running
        [[nodiscard]] size_t PreAuthSlotsInUse() const;
        /// Connections that were given a pre-auth slot since the server started (any thread); 0 when it isn't running
        [[nodiscard]] uint64_t PreAuthAdmittedConnections() const;
        /// The log lines of pre-auth connections that fail (a TLS handshake that failed or timed out, no Hello in time, a
        /// command before Hello, an oversized or unparsable Hello, a refused Hello, a stalled write): per kind of failure,
        /// the first `burst` in a window are written one by one and the rest are counted and reported in one summary line
        /// once the window has passed (see LogCoalescer). Default 10 per 10 s (real time). Before Start.
        bool SetPreAuthLogCoalescing(LogCoalescerOptions options);
        [[nodiscard]] const LogCoalescerOptions &GetPreAuthLogCoalescing() const { return _preAuthLogOptions; }
        /// Pre-auth failure lines not written one by one since the server started (any thread); 0 when it isn't running
        [[nodiscard]] uint64_t PreAuthLogLinesSuppressed() const;
        /// What Stop saw once every thread had been joined (for tests): the slots still held then (a slot a thread failed
        /// to give back, or one of a connection still queued), the connections still queued or in the hand-off slot,
        /// the slots held once those were closed, and how long the joins took
        struct StopReport
        {
            size_t slotsHeldAfterJoins = 0;
            size_t leftoverConnections = 0;
            size_t slotsHeldAfterCleanup = 0;
            std::chrono::steady_clock::duration joinTime{};
        };
        /// Called by Stop, on the thread that called it, after the joins and the cleanup and before the listening socket
        /// is closed. For tests. Not for production use; before Start.
        using StopHook = std::function<void(const StopReport &)>;
        bool SetStopHook(StopHook hook);

        // ==================== Failed-Hello rate limiting ====================

        /**
         * A host with an access token (SetAccessToken, SetTokensFile) backs off a peer that sends failed Hellos (see
         * RateLimiter for the rules): a backed-off address is accepted and closed at once, before its TLS handshake, and
         * an address with failures on record gets a shorter handshake-and-Hello budget. Loopback peers are exempt from
         * per-address backoff and, since PR S-4, have no backoff of their own (the global one that refused every
         * loopback connection, a correct token included, is gone: loopback is bounded by the pre-auth slots and the handshake
         * rate below), and (with a tokens file) a backoff per user named in Hello keeps failures from anywhere off that
         * user, which a correct token passes. A connection that ends without a successful Hello is a failure too (it
         * timed out, closed early, sent another command or an oversized frame, or failed its TLS handshake), for its
         * address only: loopback is exempt from that. On by default; a host
         * without an access token has no failed Hellos, so nothing is tracked and nothing changes. The limiter is shared by the
         * acceptor, the workers and the session thread (one mutex).
         *
         * Each setter is before Start, like SetAccessToken: false, changing nothing, while running.
         */
        bool SetRateLimiting(bool enabled);
        /// Whether a failed Hello is counted: rate limiting is on and the host has an access token
        [[nodiscard]] bool IsRateLimiting() const { return _rateLimiting && RequiresAccessToken(); }
        /// The thresholds and backoffs (RateLimitPolicy's defaults are the ones documented); mostly for tests
        bool SetRateLimitPolicy(const RateLimitPolicy &policy);
        /// The time the rate limiter reads (the real steady clock when unset), so a test can move it without sleeping
        bool SetRateLimitClock(RateLimiter::Clock clock);
        /// How the network thread learns who connected: given the accepted socket, the TCP peer's address. The default
        /// is getpeername; a test sets one to give connections made on loopback other addresses.
        using PeerAddressProvider = std::function<PeerAddress(int socket)>;
        bool SetPeerAddressProvider(PeerAddressProvider provider);
        /// Connections closed at accept because their peer was backed off (any thread), since the server was built
        [[nodiscard]] uint64_t RateLimitedConnections() const { return _rateLimitedConnections.load(); }
        /// What a Hello naming a backed-off user is answered with (a plain Error: the protocol has no code for it)
        static constexpr const char *RateLimitedError = "Too many failed attempts: try again later";

        /// Before Start, like SetAccessToken (false, changing nothing, while running): when set, the server stops (as
        /// on a Shutdown command, so IsRunning() turns false) once a client's session ends, i.e. its connection
        /// closes for any reason after the session opened. With an access token a session opens with a successful
        /// Hello, so a connection that never authenticated can't stop the server; without one, every connection is
        /// a session. A host launched by an editor sets it (N2EditorHost --exit-on-disconnect), so it doesn't
        /// outlive an editor that crashed or was killed. Off by default: the server goes back to accepting.
        bool SetStopOnDisconnect(bool stop);
        [[nodiscard]] bool StopsOnDisconnect() const { return _stopOnDisconnect; }

        /// Client-supplied text made safe for one log line: control characters (newlines included) become '?', and
        /// text over maxBytes is cut at a UTF-8 character boundary, with "..." appended
        [[nodiscard]] static std::string SanitizeForLog(std::string_view text, size_t maxBytes = 100);

        // ==================== Project and scene files (main thread) ====================

        /**
         * The project the host opened, at root (its absolute folder). Call it once ResourceUUID and ResourceLoader are
         * initialised for the project (RunHost does: the projectId namespace, ResourceLoader::Initialize(root, the
         * project's user data folder)). Enables GetProjectInfo, SetProjectSettings, SetStartupScene and the scene-file
         * commands; without a project they answer Error. Hello's projectLoaded reports it.
         */
        void SetProject(std::filesystem::path root, IO::ProjectFile project);
        [[nodiscard]] bool HasProject() const { return _project.has_value(); }
        /// The project file as last saved, or nullptr without a project
        [[nodiscard]] const IO::ProjectFile *GetProject() const { return _project ? &_project->file : nullptr; }
        [[nodiscard]] std::filesystem::path GetProjectRoot() const { return _project ? _project->root : std::filesystem::path{}; }

        /**
         * What OpenScene runs: reads the .scene file at path (a res:// path), builds it (Scene::FromJSON, validated;
         * edit mode: no component is attached) and makes it the loaded scene, with the file's asset UUID. The scene
         * revision moves on and the scene is saved at it. An Error message when there is no project, the path isn't a
         * res:// .scene path inside the assets folder, or the file can't be read or isn't a valid scene; the loaded
         * scene is kept then.
         */
        std::expected<OpenSceneInfo, std::string> OpenSceneFile(const std::string &path);
        /**
         * What NewScene runs: an empty scene named name (empty: the file's stem, or "Untitled") becomes the loaded
         * scene. With a path (a res:// .scene path that doesn't exist yet) the scene is written there first and has
         * that file; with an empty path it has none until SaveSceneToFile names one. Either way it starts saved
         * (nothing to lose). Needs a project when a path is given.
         */
        std::expected<OpenSceneInfo, std::string> NewSceneFile(const std::string &path, const std::string &name);
        /**
         * What SaveSceneToFile runs: writes the loaded scene's JSON (Scene::Serialize, indented) to path, or to the
         * scene's own file when path is empty (an Error when it has none), replacing the file atomically; also stores
         * it in SceneManager as SaveScene does. A path other than the scene's own ("save as") becomes its file, and
         * the scene takes that file's UUID. The file is indexed (its .meta written) and the saved revision becomes
         * the current one.
         */
        std::expected<OpenSceneInfo, std::string> SaveSceneFile(const std::string &path);
        /// What GetOpenScene answers; an Error message when no scene is loaded
        [[nodiscard]] std::expected<OpenSceneInfo, std::string> GetOpenSceneInfo() const;

        /**
         * The scene revision: 0 until a scene is loaded, then moved on by every command that changes the loaded scene
         * or loads another (OpenScene, NewScene, LoadScene, CreateEntity, CreateEntityEx, DestroyEntity,
         * SetEntityTransform, SetLocalTransform, SetEntityParent, SetEntityProperties, DuplicateEntity, AddComponent,
         * RemoveComponent, SetComponentFields, and Undo and Redo). It only grows, so a client refetches what it shows
         * (GetHierarchy, GetEntity) whenever it changes; undoing is a change like any other, so the revision after an
         * undo is not the revision before the edit. Each change, and each save, pushes a sceneChanged event
         * {revision, savedRevision, path}; a change adds entityIds (the objects it touched, when there are few enough)
         * or full (another scene was loaded, or a step was undone or redone by restoring a snapshot of the scene:
         * every id is invalid).
         */
        [[nodiscard]] uint32_t GetSceneRevision() const { return _sceneRevision; }
        /// The revision the loaded scene was last opened or saved at (see OpenSceneInfo::savedRevision). Undoing or
        /// redoing back to the state that was saved makes it the current revision again (no unsaved changes).
        [[nodiscard]] uint32_t GetSavedRevision() const { return _savedRevision; }

        // ==================== Undo, redo and autosave (#78, E6) ====================

        /**
         * The undo history. Every editing command records what it did (a step, or part of the open edit group), Undo
         * and Redo walk it, and opening, creating or loading a scene clears it; saving doesn't. Exposed for the host
         * and tests: a client only reaches it through the commands.
         */
        [[nodiscard]] EditHistory &GetHistory() { return _history; }
        [[nodiscard]] const EditHistory &GetHistory() const { return _history; }

        /// How often at most the autosave is written while edits keep coming (the last one waits for the host's loop,
        /// ProcessCommands). Zero writes it after every step. 2 seconds by default.
        static constexpr std::chrono::milliseconds DefaultAutosaveInterval{2000};
        void SetAutosaveInterval(const std::chrono::milliseconds interval) { _autosaveInterval = interval; }
        /// The clock the autosave interval is measured with (steady_clock::now when none is set); tests set their own
        using AutosaveClock = std::function<std::chrono::steady_clock::time_point()>;
        void SetAutosaveClock(AutosaveClock clock) { _autosaveClock = std::move(clock); }

        /// Ends every open edit group, as a closed connection or a new Hello does, and writes the autosave that is
        /// waiting. Main thread.
        void CloseEditGroups();

        /// Where the autosave of the open scene goes: <project>/.n2/autosave/ then the scene's path under assets/ (or
        /// ".untitled.scene" for a scene with no file). Empty without a project. Never the scene's own file.
        [[nodiscard]] std::filesystem::path GetAutosaveFile() const;
        /// An autosave found when a scene was opened is kept, and none is written over it, until RestoreAutosave,
        /// DiscardAutosave or a save: a client that finds it after a crash can offer it first
        [[nodiscard]] bool IsAutosaveProtected() const { return _autosaveProtected; }

        /**
         * A scene path a client sent, checked: "res://" then a path that stays inside assetsRoot (lexically, and with
         * symlinks resolved for the part that exists), whose file name ends in ".scene" (any case). The file needn't
         * exist. An Error message otherwise.
         */
        [[nodiscard]] static std::expected<ResolvedScenePath, std::string> ResolveScenePath(
            const std::filesystem::path &assetsRoot, const std::string &path);

        /// Where DeleteScene may delete scene files (usually <project>/scenes); empty disables it.
        void SetScenesDirectory(std::filesystem::path scenesDirectory) { _scenesDirectory = std::move(scenesDirectory); }
        [[nodiscard]] const std::filesystem::path& GetScenesDirectory() const { return _scenesDirectory; }

        [[nodiscard]] static bool IsPayloadLengthAllowed(uint32_t payloadLength) { return payloadLength <= MaxPayloadBytes; }
        [[nodiscard]] static bool IsViewportSizeValid(int32_t width, int32_t height);

        /// RenderFrame's pixels: reads the renderer's last frame (IRenderer::ReadFramebuffer, RGBA bottom row
        /// first on every backend) into `pixels` as width x height RGBA8 with the top row first and alpha 255,
        /// what FrameData carries. `pixels` is resized to width * height * 4 bytes (empty unless both are
        /// positive), not cleared: a backend that writes nothing leaves what was there, made opaque (black for a
        /// buffer only ever used for such a backend, as new bytes are zero). Vulkan's readback is real (its
        /// offscreen target), but it draws nothing until stages A-E of #43, so its frames are the clear colour.
        static void ReadFrame(const Renderer::Common::IRenderer &renderer, int width, int height,
                              std::vector<uint8_t> &pixels);
        /// The Error SetViewportSize and RenderFrame answer when the renderer can't render at the viewport size
        /// (Window::SetRenderSize failed): "Couldn't create a WxH render target"
        [[nodiscard]] static std::string RenderTargetError(int width, int height);

        /// The scene file a DeleteScene request names: sceneName relative to scenesDirectory, with
        /// SceneFileExtension appended unless it already ends with it (case-insensitively). nullopt unless
        /// its parent resolves (following symlinks) to scenesDirectory or a directory inside it. The file
        /// name itself isn't resolved, so a symlinked scene file means the link, not its target.
        [[nodiscard]] static std::optional<std::filesystem::path> ResolveSceneFile(
            const std::filesystem::path &scenesDirectory, const std::string &sceneName);

        /// The events PollEvents reads: log lines (the Logger subscription), and whatever a handler pushes. Push is
        /// thread-safe; see EventRing.
        [[nodiscard]] EventRing &GetEvents() { return _events; }
        [[nodiscard]] const EventRing &GetEvents() const { return _events; }

        // ==================== Editor camera and render on demand (#79, E7a) ====================

        /**
         * The editor camera (see EditorCameraState): the viewpoint of RenderFrameIfChanged's frames, owned by the server
         * (not a scene object, not saved, not in undo). SetEditorCamera replaces it; this does the same for the host
         * and tests, with the same checks, and says why a camera was refused. Setting the camera it has changes nothing.
         */
        [[nodiscard]] const EditorCameraState &GetEditorCameraState() const { return _editorCamera; }
        [[nodiscard]] std::expected<void, std::string> SetEditorCameraState(const EditorCameraState &camera);

        /// The viewport's aspect ratio, width / height (what frames and GetEditorCamera's projection use)
        [[nodiscard]] float GetViewportAspect() const
        {
            return static_cast<float>(_viewportWidth) / static_cast<float>(_viewportHeight);
        }

        /**
         * The frame revision (see FrameTracker): the number of the picture the viewport would show now. It moves on
         * when the open scene changes (any scene revision change, or another scene), the editor camera changes, the
         * viewport size changes, a RescanAssets finds changes, or the project's settings change; and RenderFrameIfChanged
         * answers a client that holds this revision without rendering. The first change after a frame was rendered
         * pushes a frameChanged {revision} event.
         */
        [[nodiscard]] uint32_t GetFrameRevision() const { return _frames.Revision(); }
        /// How many frames RenderFrameIfChanged has rendered (not counting resends of the one in the buffer), for tests
        [[nodiscard]] uint32_t GetEditorFramesRendered() const { return _editorFramesRendered; }

        /// The most ids GetEntityBounds answers at once (more is an Error)
        static constexpr std::size_t MaxBoundsEntityIds = 4096;

        // ==================== Assets and the file watcher (#81, E8) ====================

        /// The most bytes of text ReadTextAsset and WriteTextAsset carry (more is an Error)
        static constexpr std::size_t MaxTextAssetBytes = 4u * 1024u * 1024u;
        /// The most bytes of JSON text SetImportSettings accepts
        static constexpr std::size_t MaxImportSettingsBytes = 64u * 1024u;
        /// How often the watcher looks for changed asset files while a client is connected: once a second
        static constexpr std::chrono::milliseconds DefaultAssetPollInterval{1000};

        /**
         * A res:// asset path a client sent, checked: "res://" then a path of valid file names (no "..", drive, stream
         * or character a file name can't have) that stays inside assetsRoot, lexically and with symlinks resolved
         * for the part that exists. The file needn't exist. With allowRoot, "res://" (the assets folder itself) is
         * accepted. The result has the path as the file system spells it, so "res://Scripts/player.lua" on a
         * case-insensitive file system is the same asset as "res://scripts/Player.lua". An Error message otherwise.
         */
        [[nodiscard]] static std::expected<ResolvedScenePath, std::string> ResolveAssetPath(
            const std::filesystem::path &assetsRoot, const std::string &path, bool allowRoot = false);
        /// Whether one path part is a name a file or folder may have on every platform the editor runs on: not empty,
        /// at most 255 bytes, none of <>:"|?*\/ or a control character, not "." or "..", no trailing dot or space, and
        /// not a Windows device name (CON, NUL, COM1, ...), with or without an extension
        [[nodiscard]] static bool IsValidAssetName(std::string_view name);
        /// Whether ReadTextAsset and WriteTextAsset handle files with this path's extension (any case): .lua, .mat,
        /// .scene, .json, .txt, .md, .csv, .xml, .yaml, .yml, .toml, .ini, .cfg, .glsl, .vert, .frag, .shader
        [[nodiscard]] static bool IsTextAssetPath(std::string_view path);
        /// Whether text is well-formed UTF-8 (no overlong forms, surrogates, or values above U+10FFFF)
        [[nodiscard]] static bool IsValidUtf8(std::string_view text);

        /**
         * The file watcher's check, run now: when the files the asset scan looks at differ from the last scan's
         * (ResourceLoader::AssetsChangedOnDisk, which only reads the directory listing), rescans, reloads the assets the
         * engine holds in memory whose files changed (a script is reloaded in place and its LuaComponents rebuild their
         * script; any other asset is dropped from the cache, so its next use loads the file again), moves the frame
         * revision on and pushes assetsChanged. Main thread. True when it found changes. False, doing nothing, without a
         * project. ProcessCommands calls it through PollAssetsIfDue.
         */
        bool PollAssets();
        /// What ProcessCommands calls: PollAssets, but only while watching is on (SetAssetWatching), a client is
        /// connected (HasClient), the project has been opened, and the poll interval has passed since the last check. So
        /// a host with no editor attached never walks the assets folder. True when it found changes.
        bool PollAssetsIfDue();
        /// On by default; off stops PollAssetsIfDue (and so the watcher). Main thread.
        void SetAssetWatching(const bool enabled) { _watchAssets = enabled; }
        [[nodiscard]] bool IsWatchingAssets() const { return _watchAssets; }
        /// The time between the watcher's checks (DefaultAssetPollInterval); zero checks on every ProcessCommands call
        void SetAssetPollInterval(const std::chrono::milliseconds interval) { _assetPollInterval = interval; }
        [[nodiscard]] std::chrono::milliseconds GetAssetPollInterval() const { return _assetPollInterval; }
        /// The clock the poll interval is measured with (steady_clock::now when none is set); tests set their own
        using AssetPollClock = std::function<std::chrono::steady_clock::time_point()>;
        void SetAssetPollClock(AssetPollClock clock) { _assetPollClock = std::move(clock); }
        /// Whether a client is connected, which the watcher waits for. The network thread sets it as connections open and
        /// close; a test that has no socket sets it to stand in for one.
        void SetClientConnected(const bool connected) { _clientConnected = connected; }
        [[nodiscard]] bool HasClient() const { return _clientConnected; }

        /// True for a command a client is expected to poll: RenderFrame, RenderFrameIfChanged, GetAudio, PollEvents, and
        /// (ahead of the planned editor, #6, which refreshes them continuously) GetAllEntities, GetEntityTransform,
        /// GetCameraPosition, GetEditorCamera, SetEditorCamera (sent every frame of a drag), PickEntity, GetEntityBounds,
        /// ListAssets, GetAssetInfo, ReadTextAsset (the asset panel and script editor, whenever assetsChanged arrives),
        /// GetEngineHealth, GetHierarchy, GetEntity.
        /// Rule: a command a client polls never logs per call, or its lines would drown everything else (and, since
        /// every line is an event, PollEvents would always find one). Such a command still logs when it fails
        /// (ExecuteCommand's error line). No command logs a generic "issued" line any more, so this lists the handlers
        /// that must not add a per-call line of their own; a new polled command belongs here.
        [[nodiscard]] static bool IsPolledCommand(uint8_t commandType);

        // ==================== Play mode (#82, E9) ====================

        /// How long RunPlayFrame spends on one game frame and the commands that come in around it (60 frames a second)
        static constexpr std::chrono::milliseconds DefaultPlayFrameBudget{16};
        /// The most frames one Step runs
        static constexpr uint32_t MaxStepFrames = 1000;
        /// The most events one SendInput takes
        static constexpr std::size_t MaxInputEvents = 1024;

        /**
         * WritePlaySnapshot: writes the scene to play as <project>/.n2/play/<name>.scene, for a launcher to start a play
         * host from (N2EditorHost --play <file>). Play mode is a second host process (the Godot model), so this host is
         * never touched by the game: the snapshot is the open scene as it is in memory now (unsaved edits play as they
         * are; nothing is saved, no revision moves), or, for another scene path, that file as it is on disk (validated:
         * it must build as a scene). scenePath is a res:// .scene path, or empty for the open scene. Returns the
         * snapshot's absolute path (<name>-<8 hex digits of a hash of the scene's file path>.scene, so scenes of one name
         * in different folders have files of their own); an Error message without a project, without a scene to snapshot, for a bad path or
         * an unreadable or invalid scene, or on a play host.
         */
        [[nodiscard]] std::expected<std::filesystem::path, std::string> WritePlaySnapshot(const std::string &scenePath);

        /**
         * Makes this server a play host, as N2EditorHost --play does (after the project's resources are set up): reads
         * the scene snapshot at snapshotFile (Scene::FromJSON, validated) and makes it the loaded scene as a game (not
         * edit mode, so its components attach at the first frame), installs this server's keyboard as the game's
         * Input::KeySource (SendInput drives it), and starts playing (not paused). An Error message, changing nothing,
         * when the file can't be read or isn't a scene, or this server is a play host already. From now on RunPlayFrame
         * runs the game; SetPaused, Step and SendInput answer; RenderFrame draws the game; and the commands that would
         * write project files or swap the scene (OpenScene, NewScene, SaveSceneToFile, DeleteScene, SetProjectSettings,
         * SetStartupScene, RestoreAutosave, DiscardAutosave, LoadScene, and the asset writers SetImportSettings, WriteTextAsset,
         * CreateScriptAsset, CreateFolder) answer Error. The asset watcher is turned off. Main thread.
         */
        [[nodiscard]] std::expected<void, std::string> EnterPlayMode(const std::filesystem::path &snapshotFile);
        [[nodiscard]] bool IsPlayMode() const { return _playMode; }
        [[nodiscard]] bool IsPaused() const { return _paused; }
        /// How many game frames the play host has run (Step's included)
        [[nodiscard]] uint32_t GetPlayFrame() const { return _playFrame; }

        /**
         * Main thread, in a play host's loop in place of ProcessCommands: runs one game frame (none while paused; a
         * frame that throws is logged and pauses the game), then serves the commands that arrive for what is left of
         * frameBudget (at least one pass, so a game frame that takes longer than the budget doesn't starve the client).
         * Frames come at the budget's pace (60 a second by default), each measured by the real time since the one
         * before; a client sending commands as fast as it can doesn't speed them up, and the frames don't keep it from
         * being answered. Returns how many commands ran. Without play mode it is ProcessCommands(frameBudget).
         */
        size_t RunPlayFrame(std::chrono::milliseconds frameBudget = DefaultPlayFrameBudget);

        /// What a client's Shutdown runs on the main thread before the host stops: ends open edit groups and removes the
        /// autosave this host wrote for the open scene (what it didn't save is what the client chose to drop). A play
        /// host has no autosave and removes none: the files under .n2/autosave are the edit host's. Public for tests.
        void PrepareForShutdown();

        /// The commands a play host answers with an Error: the ones that write project files or swap the scene
        [[nodiscard]] static bool IsEditOnlyCommand(uint8_t commandType);

        // ==================== Read-only mode ====================

        /// What a command does to the host. Read: answers from what the host holds and changes nothing a client could
        /// save, run or lose. Write: changes the project, a scene, the game or an asset, or runs project code. Admin:
        /// controls the host itself (Shutdown).
        enum class CommandClass : uint8_t
        {
            Read,
            Write,
            Admin,
        };

        /// The class of a command id, or nullopt for an id nobody classified (every command of protocol.json is
        /// classified: a test requires it, so a new command can't ship without a decision). Not IsEditOnlyCommand,
        /// which is a play host's deny list of 13 file-writing commands.
        [[nodiscard]] static std::optional<CommandClass> ClassifyCommand(uint8_t commandType);

        /// An allow list with default deny: true only for a command classified Read. An id that is no command, or that
        /// nobody classified, is not a read.
        [[nodiscard]] static bool IsReadCommand(uint8_t commandType);

        /// Before Start (the network thread reads it), like SetAccessToken (--read-only): the host answers every
        /// command that is not a read, Shutdown included, with the readOnly error (ErrorCoded for a client that asked
        /// for it in Hello, else an Error whose message says so) and keeps serving the connection. Hello and the reads
        /// run as usual, with or without an access token. False, changing nothing, while the server is running.
        bool SetReadOnly(bool readOnly);
        [[nodiscard]] bool IsReadOnly() const { return _readOnly; }

        /// The message of a refusal: "read-only host: <command> is not allowed" (the command's name, or its id for an
        /// unknown one)
        [[nodiscard]] static std::string ReadOnlyMessage(uint8_t commandType);
        /// A command's name (as protocol.json spells it), or "command 0x<id>" for an id that is no command
        [[nodiscard]] static std::string CommandName(uint8_t commandType);

    private:
        /// A connection that is somebody's to close: whoever drops the last owner closes it (its TLS session first, then
        /// the socket). Connection's own destructor doesn't, because a Connection made around the listener must not.
        struct ConnectionCloser
        {
            void operator()(Connection *connection) const
            {
                if (connection != nullptr)
                {
                    connection->Close();
                    delete connection;
                }
            }
        };
        using ConnectionPtr = std::unique_ptr<Connection, ConnectionCloser>;

        /// One accepted connection on its way to a pre-auth worker. The slot is released after the connection is closed
        /// (members are destroyed in reverse order), so a slot is never free while its socket is still open.
        struct PreAuthJob
        {
            PreAuthSlots::Handle slot;
            ConnectionPtr connection;
            std::chrono::milliseconds helloBudget{0};
            std::chrono::milliseconds handshakeBudget{0};
        };

        /// A connection the pre-auth side is giving to the session thread, with the Hello (the whole request payload)
        /// the session replays through the ordinary Hello command; empty on a host without an access token
        struct HandOff
        {
            ConnectionPtr connection;
            std::optional<std::vector<uint8_t>> hello;
            /// The arbiter's id for the session this connection becomes
            uint64_t sessionId = 0;
        };

        /// A Hello a pre-auth worker has read and checked: the whole request payload (the session replays it), who the
        /// token says the client is, and whether it asked for ErrorCoded (for a Busy answer, before any session exists)
        struct VerifiedHello
        {
            std::vector<uint8_t> payload;
            SessionIdentity identity;
            bool errorCodes = false;
        };

        /// The acceptor thread
        void ServerLoop(int listenSocket);
        /// The summary line for the connections the acceptor closed without a word since the last one (when there are any)
        void LogAdmissionSummary(bool force);
        /// The acceptor's decision about one accepted socket: closes it, or queues it for a worker with a slot
        void AdmitAcceptedSocket(int clientSocket);
        /// A pre-auth worker thread: takes jobs until the server stops
        void PreAuthWorkerLoop();
        /// One connection on a worker: TLS, then (with an access token) one Hello, then the offer to the session
        void ServePreAuth(PreAuthJob &job);
        /// Reads and checks the one Hello frame. True with `hello` set when the connection may be offered to the session;
        /// false when it was refused or gave up (the answer sent, the outcome counted and logged).
        bool ReadAndVerifyHello(Connection &connection, std::string_view peerText, VerifiedHello &hello);
        /// Gives the connection to the session thread. The arbiter says what the candidate does to the active session:
        /// Accept (nobody is active: hand over at once); Takeover or Preempt (the active session is asked to end, and the
        /// candidate waits in its slot up to the takeover wait for it to be gone, then hands over, or is turned away);
        /// Busy (it waits the busy grace for the session to end by itself, then is turned away). A candidate that a newer
        /// takeover or preemption overtook while it waited is turned away too; one whose peer hung up gives its slot back.
        /// True when handed over; false when it was turned away (the Busy answer sent), hung up, or the server is stopping.
        /// `hello` is empty on a host without an access token (no Hello is read: the candidate is an admin with no name).
        bool OfferToSession(PreAuthJob &job, std::optional<std::vector<uint8_t>> hello, const SessionIdentity &candidate,
                            bool errorCodes);
        /// Under _sessionMutex: asks the active session (running, or still in the hand-off slot) to end
        void EndActiveSessionLocked(ConnectionPtr &dropped);
        /// Session thread: the arbiter's identity for the session follows a later Hello on its connection
        void NoteSessionIdentity(const Connection &connection);
        /// The session thread: takes one hand-off at a time until the server stops
        void SessionLoop();
        void RunSession(HandOff &handOff);
        /// Session start and end: the identity and the close request are cleared together, under the identity's mutex, so
        /// a tokens reload on the acceptor can't close the wrong session
        void BeginSessionState();
        void EndSessionState();
        /// Serves one connection until it closes. sessionOpened is set (and stays set) once the connection has a
        /// session: at once without an access token, at its first successful Hello with one.
        /// connection.connectedAt: when the connection was ready for its Hello, which the Hello deadline counts from:
        /// when it was accepted without TLS, when its TLS handshake finished with it (the handshake has its own budget).
        /// connection.helloBudget is the time from then to a good Hello.
        /// `helloReplay`: the Hello a pre-auth worker already read and verified; it is run as the connection's first
        /// request (the same path a Hello read here takes), and nothing is read for it.
        void HandleClient(Connection &connection, bool &sessionOpened,
                          std::optional<std::vector<uint8_t>> helloReplay = std::nullopt);
        void ProcessCommand(int clientSocket, uint8_t commandType, const std::vector<uint8_t> &payload);
        /// Network thread: the Logger isn't thread-safe, so log lines are posted to the main thread
        void PostLog(std::string message, bool isWarning = false);
        /// PostLog, then waits (at most 500 ms, and not once the server is stopping) for the main thread to have written
        /// the line. A pre-auth worker uses it for the line about a refused Hello, so the line is written before the
        /// refusal reaches the client, as it was when the main thread refused the Hello itself (a client, or a test, that
        /// has seen the refusal can rely on its line). The bound keeps a busy main thread from holding a worker, and the
        /// worker only waits for a line the coalescer lets through (at most its burst per window): a flood does not wait.
        void PostLogAndWait(std::string message, bool isWarning = false);
        /// Any thread: one pre-auth failure of this kind. True when its line(s) are to be written (the coalescer's
        /// decision); a summary of the kind's last window, if it ended with lines unwritten, is posted first.
        [[nodiscard]] bool AdmitPreAuthLine(std::string_view kind);
        /// Acceptor: posts the summaries of the coalescer's windows that have ended (`all`: every count, when stopping)
        void PostPreAuthLogSummaries(bool all);

        // Command handlers
        void HandleHello(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleRenderFrame(int clientSocket);
        void HandleRenderFrameIfChanged(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEditorCamera(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetEditorCamera(int clientSocket);
        void HandlePickEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetEntityBounds(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleWritePlaySnapshot(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetPaused(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleStep(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetPlayState(int clientSocket);
        void HandleSendInput(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetViewportSize(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAudio(int clientSocket);
        void HandlePollEvents(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleSetCameraPosition(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetCameraPosition(int clientSocket);

        void HandleLoadScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSaveScene(int clientSocket);
        void HandleCreateScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDeleteScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetCurrentScene(int clientSocket);

        void HandleCreateEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDestroyEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAllEntities(int clientSocket);
        void HandleGetEngineHealth(int clientSocket);
        void HandleGetEntityTransform(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleGetHierarchy(int clientSocket);
        void HandleCreateEntityEx(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityParent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetEntityProperties(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleDuplicateEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetEntity(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetLocalTransform(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleGetComponentTypes(int clientSocket);
        void HandleAddComponent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleRemoveComponent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetComponentFields(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetComponent(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetLuaFields(int clientSocket, const std::vector<uint8_t> &payload);

        void HandleCreateScript(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleRescanAssets(int clientSocket);
        void HandleListAssets(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetAssetInfo(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetImportSettings(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleReadTextAsset(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleWriteTextAsset(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleCreateScriptAsset(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleCreateFolder(int clientSocket, const std::vector<uint8_t> &payload);

        /// A rescan found changes (the RescanAssets command or the watcher): reloads the assets held in memory whose files
        /// changed, moves the frame revision on and pushes assetsChanged {added, removed, modified}. Nothing for none.
        void ApplyAssetChanges(const IO::ResourceLoader::RescanResult &changes);
        /// A project file was just written by a command: ResourceLoader::RefreshAsset, then, when it is new or changed
        /// (always, with force: a write may leave the size and the clock's tick as they were), the reload and the
        /// assetsChanged event ApplyAssetChanges gives a rescan's changes
        void NoteAssetWritten(const IO::ResourcePath &path, bool force);
        /// The engine holds this asset in memory and its file changed: a script's source is read again into the same
        /// object (the LuaComponents hold it) and its module runs again, which rebuilds the components' script instances;
        /// any other asset is dropped from the cache, so its next use loads the file. A failure is logged (a log event),
        /// never thrown. Nothing when the asset isn't held.
        void ReloadLoadedAsset(const IO::ResourcePath &path);

        void HandleOpenScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSaveSceneToFile(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleNewScene(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleGetOpenScene(int clientSocket);
        void HandleGetProjectInfo(int clientSocket);

        void HandleUndo(int clientSocket);
        void HandleRedo(int clientSocket);
        void HandleBeginEditGroup(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleEndEditGroup(int clientSocket);
        void HandleGetHistory(int clientSocket);
        void HandleGetAutosave(int clientSocket);
        void HandleRestoreAutosave(int clientSocket);
        void HandleDiscardAutosave(int clientSocket);
        void HandleSetProjectSettings(int clientSocket, const std::vector<uint8_t> &payload);
        void HandleSetStartupScene(int clientSocket, const std::vector<uint8_t> &payload);

        /// Answers a scene-file operation: SceneInfo, or Error with its message
        void SendSceneInfo(int clientSocket, const std::expected<OpenSceneInfo, std::string> &info);
        /// ProjectInfo for the project (there must be one)
        void SendProjectInfo(int clientSocket);
        /// The Error a project command answers without a project
        void SendNoProject(int clientSocket);
        /// Saves the project file with this change, then keeps it; an Error message when it can't be saved
        std::expected<void, std::string> SaveProject(IO::ProjectFile changed);

        /// The loaded scene changed (or another was loaded): moves the revision on and pushes sceneChanged.
        /// entityIds: the objects the change touched (what a client refetches); no more than MaxEventEntityIds of them
        /// go into the event, and without any (or with more) it carries none, which a client reads as "any may have
        /// changed". full: another scene was loaded, so every id a client holds is invalid.
        void MarkSceneChanged(std::vector<std::string> entityIds = {}, bool full = false);
        /// Pushes sceneChanged with the current revisions and path, and entityIds and full as MarkSceneChanged takes them
        void PushSceneChanged(std::vector<std::string> entityIds = {}, bool full = false);
        /// Answers with an Error
        void SendError(int clientSocket, const std::string &message);

        // Undo and redo
        /// Undo (true) or Redo: runs the step, moves the revision on, and answers EditResult
        void UndoOrRedo(int clientSocket, bool undo);
        /// Records an edit that was just made: a step of its own, merged into the step before it, or part of the open
        /// group (see EditHistory::Record). Pushes historyChanged for a new step, and notes that a step ended.
        void RecordEdit(std::string label, EditOp op);
        /// Pushes historyChanged {canUndo, canRedo, label, redoLabel, undoCount, redoCount}
        void PushHistoryChanged();
        /// The loaded scene rebuilt from a snapshot (Scene::Serialize text), keeping its UUID, file and edit mode:
        /// what undoing a destroy or a component removal does, so every reference is resolved again by UUID. The
        /// effect is full.
        EditOutcome RestoreSceneSnapshot(const std::string &snapshot);
        /// A snapshot of the loaded scene, for RestoreSceneSnapshot
        [[nodiscard]] static std::string SnapshotScene(const Scene &scene);

        // Autosave
        /// An edit step ended (or was undone): the autosave is due, written now unless the interval says to wait
        void NoteEditStepEnded();
        /// An edit group ended with writes that cancelled out (no step): the scene is as saved as it was before it, and
        /// an autosave written for the writes is out of date. Pushes sceneChanged, which carries the saved revision.
        void NoteGroupWithoutChange();
        /// Writes the autosave when one is due and the interval allows (or force). Removes it when the scene has no
        /// unsaved changes any more.
        void FlushAutosave(bool force = false);
        void RemoveAutosaveFile(const std::filesystem::path &file);
        /// A scene was loaded: no autosave is due, and one the file system has from before is protected
        void ResetAutosaveState();
        /// The open scene is being left (another is opened, the client shut the host down): the autosave this host
        /// wrote for it is removed, since what it holds is what the client chose to drop. One found when the scene was
        /// opened (protected) is kept: nobody has decided about it.
        void DiscardWrittenAutosave();
        /// The res:// path of the loaded scene's file, or "" when it has none (or the loaded scene isn't the one
        /// that was opened from it)
        [[nodiscard]] std::string OpenScenePath() const;
        /// Makes scene the loaded scene, with this file (empty for none), at a new revision that counts as saved
        void LoadOpenedScene(std::unique_ptr<Scene> scene, std::string path);

        // Network helpers
        /// Whose Connection it is, which decides what its waits do
        enum class EnvKind : uint8_t
        {
            /// The acceptor's wait on the listening socket: services the tokens file every slice
            Listener,
            /// A connection in a pre-auth slot: never asked to close by a reload, a short write budget, its own peer in the
            /// write-stalled line
            PreAuth,
            /// The session: closed when a reload takes its rights away, the full write budget
            Session,
        };
        /// What a Connection needs from this server: the stop and close flags, the connection clock and the write timeout.
        /// `peerText` names the peer in the write-stalled line.
        [[nodiscard]] ConnectionEnv MakeConnectionEnv(EnvKind kind, std::string peerText = {});
        /// A pre-auth worker (for every host, with or without an access token): secures a just-accepted connection when TLS is on (its session goes in connection.tls), by
        /// `deadline`; true when there is nothing to do (no TLS) or the handshake succeeded. False (the reason logged)
        /// closes the connection.
        bool BeginTls(Connection &connection, std::chrono::steady_clock::time_point deadline,
                      std::chrono::milliseconds budget);
        /// The connection clock's time (the real steady clock when none is set)
        [[nodiscard]] std::chrono::steady_clock::time_point ConnectionNow() const;
        /// Network thread: counts a connection from this peer as a failed Hello for its address (the limiter's
        /// RecordUnfinishedHello: loopback is exempt) and logs a backoff it starts. For a connection that ended without a
        /// successful Hello and without a Hello the token check already counted. A host without a limiter does nothing.
        void CountUnfinishedHello(const PeerAddress &peer);
        /// Handlers run on the main thread and don't touch the socket: this records the response frame,
        /// which ExecuteCommand returns for the network thread to send
        void SendResponse(int clientSocket, std::vector<uint8_t> data);
        /// A refusal's frame: ErrorCoded with this code for a client that asked for it, else a plain Error
        [[nodiscard]] std::vector<uint8_t> BuildDenial(std::string_view code, const std::string &message) const;

        /// Who the current connection is, as Hello decided: its user and role. On a host with no authentication nobody
        /// is named and the role is admin.
        struct ConnectionIdentity
        {
            /// Whether a Hello bound this identity (a host with authentication); false on a host without it
            bool authenticated = false;
            std::string user;
            TokenRole role = TokenRole::Admin;
            /// The token's hash for an identity from the tokens file, so a reload that rotates the token ends the connection
            TokenHash hash{};
            bool fromTokensFile = false;
        };
        /// The identity Hello would give a client with this token (and user, when it names one); nullopt when it doesn't
        /// match. Needs no authentication on a host without a token: that is an unauthenticated identity.
        [[nodiscard]] std::optional<ConnectionIdentity> Authenticate(std::string_view token, std::string_view user) const;
        /// What a Hello's token and protocol major version come to: the part of HandleHello that also runs on a pre-auth
        /// worker (it reads nothing that only the main thread owns, and logs nothing: the caller does, with `warning`).
        /// `peerSuffix` ends the refusal's log line.
        struct HelloEvaluation
        {
            enum class Result : uint8_t
            {
                Accepted,
                TokenRefused,
                VersionRefused,
            };
            Result result = Result::Accepted;
            /// The identity the token gives (Accepted)
            ConnectionIdentity identity;
            /// The Error frame of a refusal
            std::vector<uint8_t> refusal;
            /// The warning that says so (a refusal)
            std::string warning;
        };
        [[nodiscard]] HelloEvaluation EvaluateHello(const Protocol::HelloCmd &cmd, const std::string &peerSuffix) const;
        /// Whether the token (and user) a Hello payload carries would authenticate now; false for a payload that doesn't
        /// parse. The network thread asks it before a user's backoff refuses a Hello: the same Authenticate (constant
        /// time compare) the Hello handler uses, nothing is recorded.
        [[nodiscard]] bool HelloTokenIsValid(const std::vector<uint8_t> &payload) const;
        [[nodiscard]] ConnectionIdentity CurrentIdentity() const;
        void SetIdentity(ConnectionIdentity identity);
        void ClearIdentity();
        /// Acceptor thread only: re-reads the tokens file when `force` or when its signature changed, and swaps the table in
        void ReloadTokens(bool force);
        /// Ends the open connection when its user no longer has the role and token it was given. Acceptor thread (after a
        /// reload) and the session thread (right after a Hello bound an identity, in case a reload slipped in between the
        /// Hello's check and the binding).
        void CheckIdentityAgainstTokens();
        /// Acceptor thread ONLY (ReloadTokens keeps unsynchronised bookkeeping of the file): ServiceTokens runs the reload a
        /// signal asked for. The acceptor's own waits call it (every wait slice of the listener's Connection, through its
        /// beforeWait hook), so it runs every 100 ms while the host is up, whatever the session and the workers are doing:
        /// a SIGHUP revoking the session's user ends that session within about 100 ms. The workers' and the session's
        /// Connections have no beforeWait.
        void ServiceTokens();
        /// The tokens as of now (never null when there is a tokens file)
        [[nodiscard]] std::shared_ptr<const TokenTable> CurrentTokens() const;

        std::atomic<bool> _running{false};
        // The acceptor, the pre-auth workers and the session thread (see the class comment). Started by Start, joined by Stop.
        std::thread _serverThread;
        std::vector<std::thread> _workers;
        std::thread _sessionThread;
        int _listenSocket{-1};
        int _port{0};
        bool _socketsInitialized{false};
        // Set before Start only, so both threads read it without a lock
        std::string _accessToken;
        bool _readOnly{false};
        // The tokens file (set before Start only), its tokens, and what the network thread knows of the file. The table
        // is replaced whole under the mutex, by the network thread; Hello (the main thread) takes a copy of the pointer.
        std::filesystem::path _tokensFile;
        mutable std::mutex _tokensMutex;
        std::shared_ptr<const TokenTable> _tokens;
        TokensFileSignature _tokensLoaded;
        std::optional<TokensFileSignature> _tokensRejected;
        std::atomic<bool> _tokensReloadRequested{false};
        // The session's identity, written by Hello (main thread) and read by the session thread and the acceptor
        // (a tokens reload checks it); cleared with _closeConnection at the start and end of a session
        mutable std::mutex _identityMutex;
        ConnectionIdentity _identity;
        // Set when a reload took the SESSION's rights away (acceptor, or session thread): its waits fail, which ends it.
        // Also set, under _sessionMutex, when the session is to make way for a takeover or a preemption (OfferToSession).
        // Cleared with the identity at the end of a session (EndSessionState) and, under _sessionMutex, when the session
        // thread takes the next hand-off (SessionLoop): not at the start of RunSession, where it would swallow a request
        // made between the take and that start.
        std::atomic<bool> _closeConnection{false};
        // The close flag of the connections that are not the session (pre-auth workers): never set
        std::atomic<bool> _neverClose{false};
        // Whether the current connection's Hello asked for ErrorCoded: set by HandleHello (main thread), reset when a
        // connection starts and read by the network thread for Shutdown's refusal, so atomic
        std::atomic<bool> _clientErrorCodes{false};
        std::chrono::milliseconds _helloTimeout{DefaultHelloTimeout};
        std::chrono::milliseconds _tlsHandshakeTimeout{DefaultTlsHandshakeTimeout};
        // Connection deadlines: set before Start only, so the network thread reads them without a lock
        std::chrono::milliseconds _idleTimeout{DefaultIdleTimeout};
        std::chrono::milliseconds _writeTimeout{DefaultWriteTimeout};
        std::chrono::milliseconds _maxSessionAge{0};
        ConnectionClock _connectionClock;
        AcceptedSocketHook _acceptedSocketHook;
        // Rate limiting: the settings are set before Start only. The limiter is made by Start (so every run begins with
        // empty tables). It has its own mutex (every method): the acceptor, the pre-auth workers and the session thread all use it.
        bool _rateLimiting{true};
        RateLimitPolicy _rateLimitPolicy;
        RateLimiter::Clock _rateLimitClock;
        PeerAddressProvider _peerAddressProvider;
        std::optional<RateLimiter> _rateLimiter;
        std::atomic<uint64_t> _rateLimitedConnections{0};
        // Pre-auth slots (see SetPreAuthSlots). The options are set before Start; the slots, the bucket and the clock they
        // read are made by Start (every run begins empty); each has its own mutex. _slots is declared BEFORE the job queue
        // and the hand-off slot, which hold handles into it, so it is destroyed after them.
        PreAuthSlotsOptions _preAuthSlotOptions;
        std::optional<TokenBucketOptions> _handshakeRate;
        TokenBucket::Clock _handshakeClock;
        std::chrono::milliseconds _busyGrace{DefaultBusyGrace};
        std::chrono::milliseconds _admissionSummaryInterval{DefaultAdmissionSummaryInterval};
        PreHandoffHook _preHandoffHook;
        StopHook _stopHook;
        // The pre-auth failure lines' coalescer: options before Start; made by Start (every run begins empty), own mutex
        LogCoalescerOptions _preAuthLogOptions;
        std::optional<LogCoalescer> _preAuthLog;
        std::optional<PreAuthSlots> _slots;
        std::optional<TokenBucket> _handshakeBucket;
        std::atomic<uint64_t> _preAuthRefusedConnections{0};
        std::atomic<uint64_t> _busyRefusals{0};
        std::atomic<uint64_t> _sessionHandoffs{0};
        std::atomic<uint64_t> _sessionTakeovers{0};
        std::atomic<uint64_t> _sessionPreemptions{0};
        std::chrono::milliseconds _takeoverWait{DefaultTakeoverWait};
        SessionStartHook _sessionStartHook;
        SessionEndHook _sessionEndHook;
        // What the last summary line already reported (acceptor thread only)
        TokenBucket::Counters _bucketReported;
        std::chrono::steady_clock::time_point _lastAdmissionSummary{};
        // The acceptor's queue of connections for the workers: at most one per slot, so never longer than the workers
        // are many. Both guarded by _jobMutex; the condition variable is notified on a push and on Stop.
        std::mutex _jobMutex;
        std::condition_variable _jobCv;
        std::deque<PreAuthJob> _jobs;
        // The hand-off slot to the session thread (at most one connection), and whether a session is running. Guarded by
        // _sessionMutex; the condition variable is notified when the slot fills, when a session ends and on Stop. A
        // session is "active" from the moment its hand-off is taken until it has ended, so an offer sees it either in the
        // slot or running, never in neither.
        std::mutex _sessionMutex;
        std::condition_variable _sessionCv;
        std::optional<HandOff> _handoff;
        bool _sessionRunning{false};
        // Who the session is (the arbiter's id and identity from the hand-off to its end): made by Start. Every decision
        // that reads it and then changes the hand-off, the close request or the claim below is made under _sessionMutex
        // (the arbiter has its own mutex too, taken inside, never the other way round). A session ends, and the arbiter is
        // told, in the same critical section that clears _sessionRunning, so "free" and "nobody is active" are one fact.
        std::optional<SessionArbiter> _arbiter;
        // The one candidate that has been told to wait for the active session to end (it takes it over or preempts it), 0
        // when none: a newer candidate that outranks it (or is the same user) replaces the claim and the old one is turned
        // away; one that does not is judged against the claim, as if it were the active session
        uint64_t _claimId{0};
        SessionIdentity _claim;
        uint64_t _claimCounter{0};
        // The session's peer as text (empty when there is none, as for a command run without a socket), set by the session
        // thread and read by the handlers' log lines on the main thread. Pre-auth lines carry their own connection's peer.
        mutable std::mutex _peerMutex;
        std::string _peerText;
        /// " peer=<address>" for a log line about the current connection, or nothing
        [[nodiscard]] std::string PeerSuffix() const;
        void SetPeerText(std::string text);
        /// Network thread: one line for each backoff a failed Hello started (`peer` is the connection's)
        void LogRateLimitOutcome(const RateLimiter::FailureOutcome &outcome, std::string_view user,
                                 const PeerAddress &peer);
        /// What a log line may say of the user a Hello CLAIMED (client text, never trust it into a durable log): the name
        /// when it is a user of the CURRENT tokens table, "?" for any other name, "-" when nothing is claimed or the host
        /// has no named users (--token-env, no authentication). The one place every such log line goes through.
        [[nodiscard]] std::string LoggableClaimedUser(std::string_view claimed) const;
        // The connection the session thread is serving (owned by the hand-off, set for the time HandleClient runs; null
        // otherwise and for a command run without a socket). Hello (the main thread) records its outcome in
        // that connection's helloTokenRefused and helloVersionRefused, which the network thread reads once the command's
        // future is ready: the Hello is queued and awaited, so the connection outlives it.
        std::atomic<Connection *> _activeConnection{nullptr};
        // TLS: the identity is set before Start only and is read-only after it (the shared mbedtls_ssl_config; its random
        // generator is locked by Mbed TLS: MBEDTLS_THREADING_C), so the pre-auth workers handshake concurrently. A TLS
        // session is the connection's (Connection::tls), made and used by the one thread that holds the connection at the
        // time (a worker, then the session thread after the hand-off).
        std::unique_ptr<EditorTlsServer> _tlsServer;
        // Atomic: a client Shutdown clears _running on the network thread, so the setter's _running check alone
        // doesn't keep it from racing ServerLoop's read
        std::atomic<bool> _stopOnDisconnect{false};

        CommandQueue _commands;
        // Thread-safe (its own mutex): the Logger subscription pushes from whichever thread logged
        EventRing _events;
        size_t _logSubscription{0};
        // Main-thread state below
        std::vector<uint8_t> _response;
        std::filesystem::path _scenesDirectory;

        struct ProjectState
        {
            std::filesystem::path root;
            IO::ProjectFile file;
        };
        std::optional<ProjectState> _project;

        // The loaded scene's file (a res:// path, "" for none) and the Scene it was opened into: once another scene is
        // loaded (LoadScene, or anything else that switches scenes), the file is no longer the loaded scene's
        std::string _openScenePath;
        const Scene *_openScene{nullptr};
        uint32_t _sceneRevision{0};
        uint32_t _savedRevision{0};

        EditHistory _history;
        std::chrono::milliseconds _autosaveInterval{DefaultAutosaveInterval};
        std::optional<std::chrono::steady_clock::time_point> _lastAutosave;
        AutosaveClock _autosaveClock;
        bool _protectedAutosaveWarned{false};
        bool _autosavePending{false};
        bool _autosaveProtected{false};
        bool _autosaveWarned{false};

        int _viewportWidth{1280};
        int _viewportHeight{720};
        std::vector<uint8_t> _frameBuffer;

        /// What the viewport shows changed (the scene, the editor camera, the viewport size, the assets or the project's
        /// settings): moves the frame revision on, and pushes frameChanged for the first change since a frame was rendered
        void NoteViewChanged();
        /// The scene changed and the handler says so (its revision has just moved): NoteViewChanged(), and the scene as it
        /// is now is recorded as seen, so a frame is one revision on from the last, not two
        void NoteSceneChanged();
        /// Notes the open scene as it is now, and NoteViewChanged()s when it isn't the one last seen
        void ObserveScene();
        /// Renders the editor view into _frameBuffer (the open scene from the editor camera at the viewport size) and
        /// marks the frame rendered. False, with `error` set, when there is no renderer or it can't make the target.
        bool RenderEditorView(std::string &error);

        EditorCameraState _editorCamera;
        // Starts numbering at the event epoch, a random number (see FrameTracker)
        FrameTracker _frames{_events.Epoch()};
        uint32_t _editorFramesRendered{0};

        // Play mode (EnterPlayMode)
        bool _playMode{false};
        bool _paused{false};
        uint32_t _playFrame{0};
        /// The game's keys and mouse buttons, as SendInput set them (installed as the Input::KeySource in play mode)
        Input::InjectedKeys _keys;
        /// Where SendInput last put the pointer, restored before every game frame
        /// Until the first pointer event the pointer is left where the device (or the last injection) put it
        std::optional<Math::Vector2> _pointer;
        /// Runs one game frame: restores the pointer, then Application::Tick (no rendering: frames are drawn on request)
        /// with the clock's time, or exactly deltaSeconds of it
        void RunGameFrame(std::optional<double> deltaSeconds);
        void SetPausedState(bool paused);
        void PushPlayState();

        // UpdateAudio's clock; unset until its first call
        std::optional<std::chrono::steady_clock::time_point> _lastAudioUpdate;

        // The asset watcher. _clientConnected is written by the network thread and read by the main thread.
        std::atomic<bool> _clientConnected{false};
        bool _watchAssets{true};
        std::chrono::milliseconds _assetPollInterval{DefaultAssetPollInterval};
        std::optional<std::chrono::steady_clock::time_point> _lastAssetPoll;
        AssetPollClock _assetPollClock;
        bool _assetPollFailureLogged{false};
    };
}
