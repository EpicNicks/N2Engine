#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include <editor-server/Connection.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

#ifndef _WIN32
#include <fcntl.h>
#endif

using namespace N2Engine::Editor;
using namespace std::chrono_literals;

// Connection (PR S-1 of the pre-auth slots design): the socket I/O the server's network thread does, one connection at a
// time, over a real loopback socket pair with no EditorServer around it. Send, Receive and WaitUntilReady must behave
// as the EditorServer members they replaced did (poll in place of select): the deadline and the stop and close flags
// are noticed within one PollInterval, a closed peer is a failed read, and a writer that makes no progress gives up
// after the write timeout. The injected clock is a number the test moves, so none of these waits for a real deadline.

namespace
{
    /// A connected loopback pair: `client` is the test's end (blocking, with a receive timeout so a hang fails the test),
    /// `server` is the accepted end the Connection wraps (non-blocking, as the server makes every accepted socket)
    class LoopbackPair
    {
    public:
        explicit LoopbackPair(int clientReceiveBuffer = 0, int serverSendBuffer = 0)
        {
            const N2TestSockets::Handle listener = N2TestSockets::OpenTcp();
            if (listener == N2TestSockets::InvalidHandle)
                return;

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = 0;
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
            {
                N2TestSockets::Close(listener);
                return;
            }
#ifdef _WIN32
            int size = sizeof(address);
#else
            socklen_t size = sizeof(address);
#endif
            if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) != 0)
            {
                N2TestSockets::Close(listener);
                return;
            }

            _client = N2TestSockets::OpenTcp();
            if (_client == N2TestSockets::InvalidHandle)
            {
                N2TestSockets::Close(listener);
                return;
            }
            N2TestSockets::SetReceiveTimeout(_client, 10000);
            if (clientReceiveBuffer > 0)
                N2TestSockets::SetReceiveBuffer(_client, clientReceiveBuffer);
            if (!N2TestSockets::ConnectLoopback(_client, ntohs(address.sin_port)))
            {
                N2TestSockets::Close(listener);
                return;
            }
            const auto accepted = accept(listener, nullptr, nullptr);
            N2TestSockets::Close(listener);
            if (accepted == N2TestSockets::InvalidHandle)
                return;
            _server = static_cast<int>(accepted);
            if (serverSendBuffer > 0)
                N2TestSockets::SetSendBuffer(accepted, serverSendBuffer);
            MakeNonBlocking(accepted);
            _ok = true;
        }

        ~LoopbackPair()
        {
            CloseClient();
            CloseServer();
        }

        LoopbackPair(const LoopbackPair &) = delete;
        LoopbackPair &operator=(const LoopbackPair &) = delete;

        [[nodiscard]] bool Ok() const { return _ok; }
        [[nodiscard]] int ServerSocket() const { return _server; }

        bool ClientSend(const void *data, size_t size)
        {
            return N2TestSockets::SendAll(_client, static_cast<const std::uint8_t *>(data), size);
        }

        long long ClientReceive(void *out, size_t size)
        {
            return N2TestSockets::Receive(_client, static_cast<std::uint8_t *>(out), size);
        }

        void CloseClient()
        {
            if (_client != N2TestSockets::InvalidHandle)
            {
                N2TestSockets::Close(_client);
                _client = N2TestSockets::InvalidHandle;
            }
        }

        /// Closes the server's end for a test that never wrapped it in a Connection (Connection::Close does it otherwise)
        void CloseServer()
        {
            if (_server != -1)
            {
                N2TestSockets::Close(static_cast<N2TestSockets::Handle>(_server));
                _server = -1;
            }
        }

        /// The Connection took the socket over and has closed it
        void ServerClosedElsewhere() { _server = -1; }

    private:
        static void MakeNonBlocking(N2TestSockets::Handle handle)
        {
#ifdef _WIN32
            u_long enable = 1;
            ioctlsocket(handle, FIONBIO, &enable);
#else
            const int flags = fcntl(handle, F_GETFL, 0);
            fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
        }

        N2TestSockets::Handle _client = N2TestSockets::InvalidHandle;
        int _server = -1;
        bool _ok = false;
    };

    /// The server's side of a Connection's environment, with a clock the test moves
    struct Env
    {
        std::atomic<bool> running{true};
        std::atomic<bool> close{false};
        std::atomic<long long> offsetMilliseconds{0};
        std::atomic<int> waits{0};
        std::atomic<int> stalls{0};
        const std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();

        [[nodiscard]] std::chrono::steady_clock::time_point Time() const
        {
            return origin + std::chrono::milliseconds(offsetMilliseconds.load());
        }

        ConnectionEnv Make(std::chrono::milliseconds writeTimeout = 30s)
        {
            ConnectionEnv env;
            env.running = &running;
            env.closeRequested = &close;
            env.now = [this] { return Time(); };
            env.beforeWait = [this] { ++waits; };
            env.writeStalled = [this] { ++stalls; };
            env.writeTimeout = writeTimeout;
            return env;
        }
    };

    /// Waits for a future for a few real seconds; a failure here is a hang
    template <typename T>
    bool Finishes(std::future<T> &future)
    {
        return future.wait_for(5s) == std::future_status::ready;
    }
}

TEST(ConnectionTest, ReceiveAndSendMoveBytesBothWays)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    const char request[] = "hello";
    ASSERT_TRUE(pair.ClientSend(request, 5));
    char received[5] = {};
    ASSERT_TRUE(connection.Receive(received, 5));
    EXPECT_EQ(std::memcmp(received, request, 5), 0);

    const char answer[] = "ok!";
    ASSERT_TRUE(connection.Send(answer, 3));
    char back[3] = {};
    ASSERT_EQ(pair.ClientReceive(back, 3), 3);
    EXPECT_EQ(std::memcmp(back, answer, 3), 0);

    connection.Close();
    pair.ServerClosedElsewhere();
    EXPECT_EQ(connection.socket, Connection::NoSocket);
    connection.Close(); // twice is fine
}

TEST(ConnectionTest, ReceiveAssemblesAFrameThatArrivesInPieces)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    std::future<bool> read = std::async(std::launch::async,
                                        [&]
                                        {
                                            char buffer[6] = {};
                                            return connection.Receive(buffer, 6) && std::memcmp(buffer, "abcdef", 6) == 0;
                                        });
    ASSERT_TRUE(pair.ClientSend("ab", 2));
    // Two bytes are not a frame: the read is still waiting for the rest
    EXPECT_EQ(read.wait_for(250ms), std::future_status::timeout);
    ASSERT_TRUE(pair.ClientSend("cdef", 4));
    ASSERT_TRUE(Finishes(read));
    EXPECT_TRUE(read.get());
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, WaitUntilReadyReportsReadableAndWritable)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    EXPECT_TRUE(connection.WaitUntilReady(true)) << "an idle socket with room in its buffer is writable";
    ASSERT_TRUE(pair.ClientSend("x", 1));
    EXPECT_TRUE(connection.WaitUntilReady(false)) << "a byte is waiting";
    EXPECT_GE(env.waits.load(), 2) << "the before-wait hook (the token service) runs on every wait";
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, WaitUntilReadyGivesUpAtItsDeadlineOnTheInjectedClock)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    const auto deadline = env.Time() + 1s;
    std::future<bool> wait = std::async(std::launch::async, [&] { return connection.WaitUntilReady(false, deadline); });
    // The clock has not moved, so the deadline is a second away however long this takes: nothing arrives, and the wait
    // goes on slice after slice (about 2 slices here)
    EXPECT_EQ(wait.wait_for(250ms), std::future_status::timeout);
    EXPECT_GE(env.waits.load(), 1);
    env.offsetMilliseconds = 1001;
    ASSERT_TRUE(Finishes(wait)) << "noticed within a poll interval";
    EXPECT_FALSE(wait.get());

    // A deadline already passed fails at once, even with data waiting
    ASSERT_TRUE(pair.ClientSend("x", 1));
    EXPECT_FALSE(connection.WaitUntilReady(false, env.Time()));
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, TheStopFlagEndsAWaitWithinAPollInterval)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    std::future<bool> wait = std::async(std::launch::async, [&] { return connection.WaitUntilReady(false); });
    EXPECT_EQ(wait.wait_for(250ms), std::future_status::timeout);
    env.running = false;
    ASSERT_TRUE(Finishes(wait));
    EXPECT_FALSE(wait.get());

    // And a stopped server does no I/O at all
    ASSERT_TRUE(pair.ClientSend("x", 1));
    char byte = 0;
    EXPECT_FALSE(connection.Receive(&byte, 1));
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, ACloseRequestSetByTheBeforeWaitHookEndsTheWait)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    ConnectionEnv connectionEnv = env.Make();
    // What the server's token service does when a reloaded tokens file no longer grants the connection
    connectionEnv.beforeWait = [&env]
    {
        if (++env.waits >= 3)
            env.close = true;
    };
    Connection connection(pair.ServerSocket(), connectionEnv);

    char byte = 0;
    EXPECT_FALSE(connection.Receive(&byte, 1)) << "nothing was sent: only the close request can end this read";
    EXPECT_GE(env.waits.load(), 3);
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, APeerThatClosedIsAFailedReadNotAHang)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    pair.CloseClient();
    // A closed peer makes the socket readable (poll reports it, as select did), and the read then sees the end of stream
    EXPECT_TRUE(connection.WaitUntilReady(false));
    char byte = 0;
    EXPECT_FALSE(connection.Receive(&byte, 1));
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, APeerThatClosesAfterItsDataIsReadToTheEndAndThenFails)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make());

    ASSERT_TRUE(pair.ClientSend("x", 1));
    char byte = 0;
    ASSERT_TRUE(connection.Receive(&byte, 1));
    pair.CloseClient();
    EXPECT_FALSE(connection.Receive(&byte, 1));
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, ASendThatMakesNoProgressGivesUpAfterTheWriteTimeoutAndSaysSoOnce)
{
    N2TestSockets::Runtime runtime;
    // Small buffers both ways, so a few frames fill them, and a client that never reads
    LoopbackPair pair(4096, 4096);
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make(30s));

    // Several frames: Windows accepts a first non-blocking send whole however large, and only a later one finds the
    // unread data in the way
    const std::vector<std::uint8_t> frame(512 * 1024, 0x5A);
    std::future<bool> sent = std::async(std::launch::async,
                                        [&]
                                        {
                                            for (int i = 0; i < 4; ++i)
                                            {
                                                if (!connection.Send(frame.data(), frame.size()))
                                                    return false;
                                            }
                                            return true;
                                        });

    // Time passes in steps longer than the write timeout until the sender gives up (as the server's own test does)
    const auto limit = std::chrono::steady_clock::now() + 10s;
    while (sent.wait_for(20ms) != std::future_status::ready && std::chrono::steady_clock::now() < limit)
    {
        env.offsetMilliseconds += 31000;
    }
    ASSERT_TRUE(Finishes(sent));
    EXPECT_FALSE(sent.get());
    EXPECT_EQ(env.stalls.load(), 1) << "one line per close";
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, ACloseRequestDuringASendIsNotAStallAndLogsNothing)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair(4096, 4096);
    ASSERT_TRUE(pair.Ok());
    Env env;
    Connection connection(pair.ServerSocket(), env.Make(30s));

    const std::vector<std::uint8_t> frame(512 * 1024, 0x5A);
    std::future<bool> sent = std::async(std::launch::async,
                                        [&]
                                        {
                                            for (int i = 0; i < 4; ++i)
                                            {
                                                if (!connection.Send(frame.data(), frame.size()))
                                                    return false;
                                            }
                                            return true;
                                        });
    EXPECT_EQ(sent.wait_for(300ms), std::future_status::timeout) << "blocked on a client that never reads";
    env.close = true;
    env.offsetMilliseconds += 31000; // past the write deadline too: the close request still comes first
    ASSERT_TRUE(Finishes(sent));
    EXPECT_FALSE(sent.get());
    EXPECT_EQ(env.stalls.load(), 0);
    connection.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, TheDestructorLeavesASocketItDoesNotOwnOpen)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair pair;
    ASSERT_TRUE(pair.Ok());
    Env env;
    {
        // As the accept loop wraps the listener to wait on it: never closed by the wrapper
        Connection around(pair.ServerSocket(), env.Make());
        ASSERT_TRUE(pair.ClientSend("x", 1));
        EXPECT_TRUE(around.WaitUntilReady(false));
    }
    char byte = 0;
    Connection again(pair.ServerSocket(), env.Make());
    EXPECT_TRUE(again.Receive(&byte, 1)) << "the socket is still open and still has its byte";
    EXPECT_EQ(byte, 'x');
    again.Close();
    pair.ServerClosedElsewhere();
}

TEST(ConnectionTest, ItsHelloOutcomeFlagsStartClearAndStayPerConnection)
{
    N2TestSockets::Runtime runtime;
    LoopbackPair first;
    LoopbackPair second;
    ASSERT_TRUE(first.Ok());
    ASSERT_TRUE(second.Ok());
    Env env;
    Connection a(first.ServerSocket(), env.Make());
    Connection b(second.ServerSocket(), env.Make());
    EXPECT_FALSE(a.helloTokenRefused.load());
    EXPECT_FALSE(a.helloVersionRefused.load());
    a.helloTokenRefused = true;
    EXPECT_FALSE(b.helloTokenRefused.load());
    a.Close();
    first.ServerClosedElsewhere();
    b.Close();
    second.ServerClosedElsewhere();
}
