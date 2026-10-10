#pragma once

// The few socket calls the tests make on loopback, under one set of names on Windows (Winsock) and POSIX. Include it
// after the engine headers: <windows.h> macros (near, far, ...) must not reach them.

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace N2TestSockets
{
#ifdef _WIN32
    using Handle = SOCKET;
    constexpr Handle InvalidHandle = INVALID_SOCKET;
#else
    using Handle = int;
    constexpr Handle InvalidHandle = -1;
#endif

    /// Winsock needs WSAStartup before the first socket and WSACleanup after the last; a no-op elsewhere
    class Runtime
    {
    public:
        Runtime()
        {
#ifdef _WIN32
            WSADATA wsaData;
            _started = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#endif
        }

        ~Runtime()
        {
#ifdef _WIN32
            if (_started)
                WSACleanup();
#endif
        }

        Runtime(const Runtime &) = delete;
        Runtime& operator=(const Runtime &) = delete;

    private:
#ifdef _WIN32
        bool _started = false;
#endif
    };

    inline Handle OpenTcp()
    {
        return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    inline void Close(Handle handle)
    {
#ifdef _WIN32
        closesocket(handle);
#else
        close(handle);
#endif
    }

    /// A hung server fails a receive after this long instead of blocking the test
    inline void SetReceiveTimeout(Handle handle, int milliseconds)
    {
#ifdef _WIN32
        DWORD timeout = static_cast<DWORD>(milliseconds);
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        timeval timeout{};
        timeout.tv_sec = milliseconds / 1000;
        timeout.tv_usec = (milliseconds % 1000) * 1000;
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    }

    inline bool ConnectLoopback(Handle handle, int port)
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        return connect(handle, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    /// True if all of the bytes were sent
    inline bool SendAll(Handle handle, const std::uint8_t *data, std::size_t size)
    {
#ifdef _WIN32
        return send(handle, reinterpret_cast<const char*>(data), static_cast<int>(size), 0) == static_cast<int>(size);
#else
        // MSG_NOSIGNAL: a send to a connection the server closed is a failed send, not a SIGPIPE that ends the run
        return send(handle, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
#endif
    }

    /// Bytes received (0 once the peer closed), or -1 on an error or a receive timeout
    inline long long Receive(Handle handle, std::uint8_t *out, std::size_t size)
    {
#ifdef _WIN32
        return recv(handle, reinterpret_cast<char*>(out), static_cast<int>(size), 0);
#else
        return recv(handle, out, size, 0);
#endif
    }

    /// True if the last failed call failed because a receive timed out
    inline bool LastErrorWasTimeout()
    {
#ifdef _WIN32
        return WSAGetLastError() == WSAETIMEDOUT;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
#endif
    }

    inline std::size_t BytesAvailable(Handle handle)
    {
#ifdef _WIN32
        u_long available = 0;
        ioctlsocket(handle, FIONREAD, &available);
#else
        int available = 0;
        ioctl(handle, FIONREAD, &available);
#endif
        return static_cast<std::size_t>(available);
    }

    /// The address of the other end, or an empty string
    inline std::string PeerAddress(Handle handle)
    {
        sockaddr_in peer{};
#ifdef _WIN32
        int size = sizeof(peer);
#else
        socklen_t size = sizeof(peer);
#endif
        if (getpeername(handle, reinterpret_cast<sockaddr*>(&peer), &size) != 0)
            return {};
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
        return text;
    }
}
