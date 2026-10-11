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

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;

// A client that vanishes while the host still has answers to write must cost the host that one connection and
// nothing else. On Linux a write to a closed connection raises SIGPIPE, whose default action ends the process, so
// there a regression ends this whole test program instead of failing one test.
//
// What this can and cannot show: no command has a large answer, so it queues many small requests and resets or
// closes before the host answers any, so every answer is written to a dead connection. It is a best-effort
// reproducer: it does not prove a single send was cut short mid-frame, and if the kernel reports ECONNRESET on the
// first send after the RST (no SIGPIPE) it passes even without MSG_NOSIGNAL and SIG_IGN. Whether it fails with the
// fix reverted is still to be checked once a Linux CI leg exists.

namespace
{
    class Client
    {
    public:
        explicit Client(const int port)
        {
            _socket = N2TestSockets::OpenTcp();
            if (_socket == N2TestSockets::InvalidHandle)
                return;
            _connected = N2TestSockets::ConnectLoopback(_socket, port);
        }

        ~Client()
        {
            Close(false);
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

            return N2TestSockets::SendAll(_socket, all.data(), all.size());
        }

        /// Closes now. `abortive` resets the connection (RST) instead of closing it politely (FIN), so any answer
        /// still in flight meets a connection that no longer exists
        void Close(const bool abortive)
        {
            if (_socket == N2TestSockets::InvalidHandle)
                return;
            if (abortive)
                N2TestSockets::AbortOnClose(_socket);
            N2TestSockets::Close(_socket);
            _socket = N2TestSockets::InvalidHandle;
        }

    private:
        N2TestSockets::Runtime _runtime;
        N2TestSockets::Handle _socket = N2TestSockets::InvalidHandle;
        bool _connected = false;
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
            // Far more than one answer. The host's main loop is not run yet, so none is written while the client is
            // connected: the requests only queue up, and the client goes away before the first answer
            ASSERT_TRUE(client.SendRequests(400));
            client.Close(abortive);
        }
        // The host keeps writing answers to the connection that no longer exists
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
