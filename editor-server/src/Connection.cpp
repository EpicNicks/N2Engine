#include "editor-server/Connection.hpp"

#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace N2Engine::Editor
{
    namespace
    {
#ifdef _WIN32
        int LastSocketError()
        {
            return WSAGetLastError();
        }

        void CloseNativeSocket(const int socket)
        {
            closesocket(static_cast<SOCKET>(socket));
        }
#else
        int LastSocketError()
        {
            return errno;
        }

        void CloseNativeSocket(const int socket)
        {
            close(socket);
        }
#endif

#ifdef MSG_NOSIGNAL
        // A write to a connection the client already dropped is an error return, never a SIGPIPE that kills the host
        constexpr int SendFlags = MSG_NOSIGNAL;
#else
        constexpr int SendFlags = 0;
#endif

        /// Whether a failed send or recv only means "not now" on a non-blocking socket (or was interrupted): wait again
        bool IsWouldBlock(const int error)
        {
#ifdef _WIN32
            return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
            return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
        }

        /// One wait of at most Connection::PollInterval on one socket. Positive when it is ready (a pending error or a
        /// closed peer counts: the next recv or send reports it, as it did for select), 0 on timeout, negative on an
        /// error (including a socket that isn't one, POLLNVAL, which select reported as an error too).
        ///
        /// A wait interrupted by a signal (EINTR; WSAEINTR on Windows) is NOT an error: it reports 0, like a timeout, so
        /// the caller looks at its flags and deadline and waits again. With one thread this could be told apart from a
        /// failure only by luck (the host's SIGHUP handler runs on whichever thread the kernel picks); with an acceptor,
        /// pre-auth workers and a session thread the signal lands on any of them and would otherwise end an innocent
        /// connection, so the interruption is retried everywhere.
        int PollOnce(const int socket, const bool forWrite)
        {
            const int timeoutMilliseconds = static_cast<int>(Connection::PollInterval.count());
#ifdef _WIN32
            WSAPOLLFD descriptor{};
            descriptor.fd = static_cast<SOCKET>(socket);
            descriptor.events = forWrite ? POLLWRNORM : POLLRDNORM;
            const int result = WSAPoll(&descriptor, 1, timeoutMilliseconds);
            if (result == SOCKET_ERROR)
                return WSAGetLastError() == WSAEINTR ? 0 : -1;
#else
            pollfd descriptor{};
            descriptor.fd = socket;
            descriptor.events = forWrite ? POLLOUT : POLLIN;
            const int result = poll(&descriptor, 1, timeoutMilliseconds);
            if (result < 0)
                return errno == EINTR ? 0 : -1;
#endif
            if (result == 0)
                return 0;
            return (descriptor.revents & POLLNVAL) != 0 ? -1 : 1;
        }
    }

    Connection::Connection(const int socketHandle, ConnectionEnv connectionEnv)
        : socket(socketHandle), env(std::move(connectionEnv))
    {
    }

    Connection::~Connection() = default;

    std::chrono::steady_clock::time_point Connection::Now() const
    {
        return env.now ? env.now() : std::chrono::steady_clock::now();
    }

    void Connection::EndTls()
    {
        if (tls)
        {
            tls->Close();
        }
        tls.reset();
    }

    void Connection::Close()
    {
        EndTls();
        if (socket != NoSocket)
        {
            CloseNativeSocket(socket);
            socket = NoSocket;
        }
    }

    bool Connection::Send(const void *data, const size_t size)
    {
        size_t sent = 0;
        auto *bytes = static_cast<const char *>(data);

        // A client that stops reading must not hold the server for ever: the write fails when it makes no progress for the
        // write timeout. Progress (bytes written, or the socket becoming writable again) restarts the wait, so a slow
        // client that does read a large frame is not cut off. The one log line per close is written by the server.
        auto deadline = Now() + env.writeTimeout;
        const auto restart = [&] { deadline = Now() + env.writeTimeout; };
        const auto logIfStalled = [&]
        {
            if (env.running->load() && !env.closeRequested->load() && Now() >= deadline && env.writeStalled)
            {
                env.writeStalled();
            }
        };

        if (tls)
        {
            while (sent < size)
            {
                size_t count = 0;
                switch (tls->Write(bytes + sent, size - sent, count))
                {
                case TlsStep::Done:
                    sent += count;
                    if (count > 0) restart();
                    break;
                case TlsStep::WantWrite: // the same bytes are offered again, as the library asks
                    if (!WaitUntilReady(true, deadline))
                    {
                        logIfStalled();
                        return false;
                    }
                    restart(); // writable again: the client took some of what was queued
                    break;
                case TlsStep::WantRead: // only a renegotiation needs this, and it isn't enabled
                case TlsStep::Failed:
                    return false;
                }
            }
            return true;
        }

        while (sent < size)
        {
            if (!WaitUntilReady(true, deadline))
            {
                logIfStalled();
                return false;
            }
            const int result = static_cast<int>(send(socket, bytes + sent, static_cast<int>(size - sent), SendFlags));
            if (result < 0 && IsWouldBlock(LastSocketError())) continue; // writable a moment ago, not now: wait again
            if (result <= 0) return false;
            sent += result;
            restart();
        }
        return true;
    }

    bool Connection::Receive(void *data, const size_t size,
                             const std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        size_t received = 0;
        auto *bytes = static_cast<char *>(data);

        if (tls)
        {
            while (received < size)
            {
                // Returns at once for bytes the TLS library has already decrypted (see WaitUntilReady), and otherwise
                // when the socket has more of a record
                if (!WaitUntilReady(false, deadline)) return false;
                size_t count = 0;
                switch (tls->Read(bytes + received, size - received, count))
                {
                case TlsStep::Done:
                    received += count;
                    break;
                case TlsStep::WantRead: // a partial record: wait for the rest
                    break;
                case TlsStep::WantWrite:
                    if (!WaitUntilReady(true, deadline)) return false;
                    break;
                case TlsStep::Failed:
                    return false;
                }
            }
            return true;
        }

        while (received < size)
        {
            if (!WaitUntilReady(false, deadline)) return false;
            const int result = static_cast<int>(recv(socket, bytes + received, static_cast<int>(size - received), 0));
            if (result < 0 && IsWouldBlock(LastSocketError())) continue; // readable a moment ago, not now: wait again
            if (result <= 0) return false;
            received += result;
        }
        return true;
    }

    bool Connection::WaitUntilReady(const bool forWrite,
                                    const std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        while (env.running->load())
        {
            // Everything the serving thread waits on goes through here, so this is where it notices a SIGHUP (the tokens
            // file re-read) and where a connection whose rights were revoked ends
            if (env.beforeWait)
                env.beforeWait();
            if (env.closeRequested->load())
                return false;

            if (deadline.has_value() && Now() >= *deadline)
                return false;

            // Bytes the TLS library has already taken off the socket and decrypted are invisible to poll(): without
            // this a frame that arrived in the same record as the one before it would wait for more traffic
            if (!forWrite && tls && tls->HasBufferedInput())
                return true;

            const int result = PollOnce(socket, forWrite);
            if (result > 0) return true;
            if (result < 0) return false;
        }
        return false;
    }
}
