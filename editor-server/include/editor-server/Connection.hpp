#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include "editor-server/EditorTls.hpp"
#include "editor-server/RateLimiter.hpp"

namespace N2Engine::Editor
{
    /// What a Connection's waits need from the server around it. Every member is read on the thread that uses the
    /// Connection; the two flags are the server's own atomics (both required) and must outlive the Connection.
    struct ConnectionEnv
    {
        /// The server's running flag: every wait gives up once it is clear, so Stop() can't hang on a socket
        const std::atomic<bool> *running = nullptr;
        /// Set when the connection must end (its rights were revoked): its waits fail
        const std::atomic<bool> *closeRequested = nullptr;
        /// The connection clock (every deadline is read from it); the real steady clock when empty
        std::function<std::chrono::steady_clock::time_point()> now;
        /// Run at the start of every wait slice, before anything else (the server services a SIGHUP token reload here).
        /// May set closeRequested. Empty does nothing.
        std::function<void()> beforeWait;
        /// Called once when a Send gave up because the write made no progress for writeTimeout (and the server is still
        /// running and the connection wasn't asked to close): the server writes its one log line
        std::function<void()> writeStalled;
        /// A Send fails when it makes no progress for this long
        std::chrono::milliseconds writeTimeout{30000};
    };

    /**
     * One accepted connection: its socket, its TLS session when there is one, who it is from, and when it started.
     * Send, Receive and WaitUntilReady are the only places that touch the socket after accept. Everything here is used
     * from the one thread that serves the connection; the Hello outcome flags are atomic because the main thread sets
     * them while it runs the Hello.
     *
     * The socket is held as an int (on Windows a SOCKET is narrowed to int at accept, as it always was). Close() ends
     * the TLS session and then closes the socket; the destructor does not close it, so a Connection made around a
     * socket somebody else owns (the listener, to wait for a pending accept) is never closed by mistake.
     */
    struct Connection
    {
        static constexpr int NoSocket = -1;
        /// How long one wait slice lasts: how often a waiting thread looks at the stop flag, the close flag and the
        /// deadline
        static constexpr std::chrono::milliseconds PollInterval{100};

        Connection(int socketHandle, ConnectionEnv connectionEnv);
        ~Connection();
        Connection(const Connection &) = delete;
        Connection &operator=(const Connection &) = delete;

        /// Sends all of the data; false when the write made no progress for the write timeout, on an error, or when the
        /// server is stopping or the connection was asked to close
        bool Send(const void *data, size_t size);
        /// Reads exactly `size` bytes. False on disconnect, error, a stopping server, a close request, or (when given)
        /// once the deadline has passed.
        bool Receive(void *data, size_t size,
                     std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);
        /// Waits (in PollInterval slices) until the socket is readable or writable, or, for a read with TLS, until the
        /// library holds decrypted bytes. False once the server is stopping, the connection was asked to close, the
        /// deadline (if any) has passed, or on error. Keeps every socket call non-blocking, so Stop() can't hang.
        bool WaitUntilReady(bool forWrite, std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

        /// Whether the peer has gone: it closed or reset the connection (a socket error counts). Looks at the socket
        /// without reading anything, so a client that is still there, or has sent bytes, is not gone. Where the OS says so
        /// (Linux: POLLRDHUP) a half-close is noticed even with bytes unread; elsewhere a peer that sent something and then
        /// closed is noticed only once the bytes are read. With TLS only the transport is looked at (a client's
        /// close_notify is bytes), so a TLS client that said goodbye is not seen until it is read. Non-blocking.
        [[nodiscard]] bool PeerHasHungUp() const;

        /// TCP keepalive on a socket: SO_KEEPALIVE, and the first probe after `idle` of silence, then one every
        /// `interval`, the peer being dead after `count` unanswered. The options differ per OS: Linux TCP_KEEPIDLE,
        /// TCP_KEEPINTVL and TCP_KEEPCNT; macOS TCP_KEEPALIVE (the idle time), TCP_KEEPINTVL and TCP_KEEPCNT; Windows
        /// SIO_KEEPALIVE_VALS (idle and interval in milliseconds; the number of probes is not settable there, the system
        /// uses 10). False when an option could not be set (what was set stays).
        static bool EnableKeepAlive(int socketHandle, std::chrono::seconds idle, std::chrono::seconds interval, int count);
        /// The values the host uses for a session: 60 s idle, then a probe every 10 s, 3 unanswered: a half-open
        /// connection is dropped by the kernel after about 90 s (about 160 s on Windows)
        static constexpr std::chrono::seconds KeepAliveIdle{60};
        static constexpr std::chrono::seconds KeepAliveInterval{10};
        static constexpr int KeepAliveCount = 3;

        /// Ends the TLS session, if any (before the socket is closed)
        void EndTls();
        /// EndTls, then closes the socket; safe to call twice
        void Close();

        /// The connection clock's time
        [[nodiscard]] std::chrono::steady_clock::time_point Now() const;

        int socket;
        ConnectionEnv env;
        /// Null without TLS, and before the handshake starts
        std::unique_ptr<EditorTlsSession> tls;
        PeerAddress peer;
        /// When the server took the connection off the listener (the TLS handshake's budget counts from here)
        std::chrono::steady_clock::time_point acceptedAt{};
        /// When the connection was ready for its Hello: accept without TLS, the end of the handshake with it
        std::chrono::steady_clock::time_point connectedAt{};
        /// The time a gated connection has from connectedAt to send a good Hello
        std::chrono::milliseconds helloBudget{0};
        /// The arbiter's id of the session this connection is (0 before it is handed to the session thread); set and read
        /// by the session thread only
        uint64_t sessionId = 0;
        /// Whether the last Hello on this connection was refused for its token (as opposed to its version or its
        /// shape): the session thread clears it before queueing a Hello and reads it once the command's future is
        /// ready; the main thread sets it
        std::atomic<bool> helloTokenRefused{false};
        /// Whether the last Hello was refused only for its protocol version (the token was right): not counted
        std::atomic<bool> helloVersionRefused{false};
    };
}
