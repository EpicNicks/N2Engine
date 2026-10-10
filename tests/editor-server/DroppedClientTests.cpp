#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

#ifdef _WIN32
// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
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
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;

// A client that vanishes while the host still has answers to write must cost the host that one connection and
// nothing else. On Linux a write to a closed connection raises SIGPIPE, whose default action ends the process, so
// there a regression ends this whole test program instead of failing one test.

namespace
{
#ifdef _WIN32
    using RawSocket = SOCKET;
    constexpr RawSocket NoSocket = INVALID_SOCKET;
#else
    using RawSocket = int;
    constexpr RawSocket NoSocket = -1;
#endif

    class Client
    {
    public:
        explicit Client(const int port)
        {
#ifdef _WIN32
            WSADATA wsaData;
            _wsaStarted = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#endif
            _socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_socket == NoSocket)
                return;
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
            _connected = ::connect(_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        }

        ~Client()
        {
            Close(false);
#ifdef _WIN32
            if (_wsaStarted)
                WSACleanup();
#endif
        }

        Client(const Client &) = delete;
        Client& operator=(const Client &) = delete;

        [[nodiscard]] bool IsConnected() const { return _connected; }

        /// Sends `count` copies of a request the host answers with a (small) error frame
        bool SendRequests(const int count)
        {
            BufferWriter payload;
            payload.WriteString("00000000-0000-0000-0000-000000000000");
            BufferWriter one;
            one.WriteU8(static_cast<uint8_t>(CommandType::DestroyEntity));
            one.WriteU32(static_cast<uint32_t>(payload.Size()));
            one.WriteBytes(payload.Data());

            std::vector<uint8_t> all;
            for (int i = 0; i < count; ++i)
                all.insert(all.end(), one.Data().begin(), one.Data().end());

            size_t sent = 0;
            while (sent < all.size())
            {
                const auto result = ::send(_socket, reinterpret_cast<const char*>(all.data() + sent),
                                           static_cast<int>(all.size() - sent), 0);
                if (result <= 0)
                    return false;
                sent += static_cast<size_t>(result);
            }
            return true;
        }

        /// Closes now. `abortive` resets the connection (RST) instead of closing it politely (FIN), so any answer
        /// still in flight meets a connection that no longer exists
        void Close(const bool abortive)
        {
            if (_socket == NoSocket)
                return;
            if (abortive)
            {
                linger option{};
                option.l_onoff = 1;
                option.l_linger = 0;
                ::setsockopt(_socket, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&option), sizeof(option));
            }
#ifdef _WIN32
            ::closesocket(_socket);
#else
            ::close(_socket);
#endif
            _socket = NoSocket;
        }

    private:
        RawSocket _socket = NoSocket;
        bool _connected = false;
#ifdef _WIN32
        bool _wsaStarted = false;
#endif
    };

    /// Plays the host's main loop for a while, answering whatever the network thread has queued
    void Drain(EditorServer &server, const std::chrono::milliseconds duration)
    {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end)
            server.ProcessCommands(std::chrono::milliseconds(10));
    }

    void RunDroppedClient(const bool abortive)
    {
        EditorServer server;
        ASSERT_TRUE(server.Start(0));
        {
            Client client(server.GetPort());
            ASSERT_TRUE(client.IsConnected());
            // Far more than one answer, so most of them are written after the client is gone
            ASSERT_TRUE(client.SendRequests(400));
            client.Close(abortive);
        }
        Drain(server, std::chrono::milliseconds(1500));

        // Still alive, and still serving the next client
        EXPECT_TRUE(server.IsRunning());
        Client next(server.GetPort());
        EXPECT_TRUE(next.IsConnected());
        server.Stop();
        EXPECT_FALSE(server.IsRunning());
    }
}

TEST(EditorServerDroppedClientTest, AClientThatResetsWithAnswersPendingDoesNotKillTheHost)
{
    RunDroppedClient(true);
}

TEST(EditorServerDroppedClientTest, AClientThatClosesWithAnswersPendingDoesNotKillTheHost)
{
    RunDroppedClient(false);
}
