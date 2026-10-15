#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/RateLimiter.hpp>
#include <engine/Logger.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;

// The deadlines that keep one connection from holding the one-at-a-time host (audit F1 and F2): the 2 s budget before
// Hello and its accounting, then the idle, write and session deadlines of a session. Real loopback sockets to a real
// EditorServer; every deadline reads the injected connection clock, so a test moves time by setting a number and the
// server notices within one poll interval (0.1 s of real time). Nothing here sleeps. The pre-Hello accounting of a TLS
// handshake that stalls is in EditorTlsTests.cpp; the tables themselves are in RateLimiterTests.cpp.

namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr const char *Token = "deadline-test-token";

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;
    };

    Frame Decode(const std::vector<uint8_t> &frame)
    {
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const uint32_t size = r.ReadU32();
        const auto body = r.ReadBytes(size);
        out.payload.assign(body.begin(), body.end());
        return out;
    }

    std::vector<uint8_t> HelloPayload(const std::string &token)
    {
        BufferWriter w;
        w.WriteString("deadline test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        return w.Release();
    }

    std::vector<uint8_t> PollPayload()
    {
        BufferWriter w;
        w.WriteU32(0); // epoch
        w.WriteU32(0); // afterSeq
        w.WriteU32(10); // maxEvents
        return w.Release();
    }

    /// The Logger's lines while this object lives
    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
            {
                const std::lock_guard lock(_mutex);
                _lines.emplace_back(message);
            };
        }

        ~LogCapture() { Logger::logEvent -= _id; }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] std::vector<std::string> Lines() const
        {
            const std::lock_guard lock(_mutex);
            return _lines;
        }

        [[nodiscard]] size_t Count(const std::string_view text) const
        {
            size_t count = 0;
            for (const std::string &line : Lines())
            {
                if (line.find(text) != std::string::npos)
                    ++count;
            }
            return count;
        }

        [[nodiscard]] bool Has(const std::string_view text) const { return Count(text) > 0; }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        size_t _id = 0;
    };

    class TestClient
    {
    public:
        /// receiveBufferBytes: a small receive buffer, set before connecting so the TCP window starts small too
        explicit TestClient(int port, int receiveBufferBytes = 0)
        {
            _socket = N2TestSockets::OpenTcp();
            if (_socket == N2TestSockets::InvalidHandle)
                return;
            N2TestSockets::SetReceiveTimeout(_socket, 10000); // a hung server fails the test instead of blocking it
            if (receiveBufferBytes > 0)
                N2TestSockets::SetReceiveBuffer(_socket, receiveBufferBytes);
            _connected = N2TestSockets::ConnectLoopback(_socket, port);
        }

        ~TestClient()
        {
            if (_socket != N2TestSockets::InvalidHandle)
                N2TestSockets::Close(_socket);
        }

        TestClient(const TestClient &) = delete;
        TestClient &operator=(const TestClient &) = delete;

        [[nodiscard]] bool IsConnected() const { return _connected; }
        [[nodiscard]] N2TestSockets::Handle Socket() const { return _socket; }

        bool SendRequest(CommandType type, const std::vector<uint8_t> &payload)
        {
            BufferWriter w;
            w.WriteU8(static_cast<uint8_t>(type));
            w.WriteU32(static_cast<uint32_t>(payload.size()));
            w.WriteBytes(payload);
            return N2TestSockets::SendAll(_socket, w.Data().data(), w.Size());
        }

        [[nodiscard]] size_t BytesAvailable() const { return N2TestSockets::BytesAvailable(_socket); }

        /// Up to `size` bytes (at least one unless the peer closed or the 10 s receive timeout passed): the count, 0 once
        /// the peer closed, negative on an error
        long long ReadSome(uint8_t *out, size_t size) { return N2TestSockets::Receive(_socket, out, size); }

        std::optional<std::vector<uint8_t>> ReadResponse()
        {
            std::vector<uint8_t> frame(5);
            if (!ReadExact(frame.data(), 5))
                return std::nullopt;
            uint32_t size = 0;
            std::memcpy(&size, frame.data() + 1, sizeof(size));
            frame.resize(5 + size);
            if (size > 0 && !ReadExact(frame.data() + 5, size))
                return std::nullopt;
            return frame;
        }

        /// True when the server closed (or reset) the connection within `milliseconds`; a byte it sent doesn't count
        bool IsClosedWithin(int milliseconds)
        {
            N2TestSockets::SetReceiveTimeout(_socket, milliseconds);
            std::uint8_t byte = 0;
            const long long result = N2TestSockets::Receive(_socket, &byte, 1);
            const bool closed = result == 0 || (result < 0 && !N2TestSockets::LastErrorWasTimeout());
            N2TestSockets::SetReceiveTimeout(_socket, 10000);
            return closed;
        }

    private:
        bool ReadExact(uint8_t *out, size_t count)
        {
            size_t received = 0;
            while (received < count)
            {
                const long long result = N2TestSockets::Receive(_socket, out + received, count - received);
                if (result <= 0)
                    return false;
                received += static_cast<size_t>(result);
            }
            return true;
        }

        N2TestSockets::Runtime _runtime; // before the socket: Winsock must be started first and stopped last
        bool _connected = false;
        N2TestSockets::Handle _socket = N2TestSockets::InvalidHandle;
    };

    /// Sends a request and plays the host's main loop until its response arrives
    std::optional<Frame> Roundtrip(EditorServer &server, TestClient &client, const CommandType type,
                                   const std::vector<uint8_t> &payload = {})
    {
        if (!client.SendRequest(type, payload))
            return std::nullopt;
        const auto limit = std::chrono::steady_clock::now() + 10s;
        while (client.BytesAvailable() < 5 && std::chrono::steady_clock::now() < limit)
        {
            server.ProcessCommands(10ms);
        }
        const auto response = client.ReadResponse();
        if (!response.has_value())
            return std::nullopt;
        return Decode(*response);
    }

    std::string DumpLines(const LogCapture &log)
    {
        std::string text;
        for (const std::string &line : log.Lines())
        {
            text += line.substr(0, 200) + "
";
        }
        return text;
    }

    /// Plays the host's main loop until the log has this text (the network thread posts its lines to it)
    bool PumpUntilLogged(EditorServer &server, const LogCapture &log, const std::string_view text)
    {
        const auto limit = std::chrono::steady_clock::now() + 10s;
        while (!log.Has(text) && std::chrono::steady_clock::now() < limit)
        {
            server.ProcessCommands(10ms);
        }
        return log.Has(text);
    }

    void StopWithin(EditorServer &server, std::chrono::seconds limit)
    {
        std::promise<void> stopped;
        std::future<void> stoppedFuture = stopped.get_future();
        std::thread stopper(
            [&]
            {
                server.Stop();
                stopped.set_value();
            });
        if (stoppedFuture.wait_for(limit) != std::future_status::ready)
        {
            std::fprintf(stderr, "EditorServer::Stop() hung with a client connected\n");
            std::abort(); // a hung thread can't be joined
        }
        stopper.join();
    }

    /// The address the provider hands the network thread: the test sets it between connections
    class SharedPeer
    {
    public:
        explicit SharedPeer(const PeerAddress &initial) : _address(initial) {}

        void Set(const PeerAddress &address)
        {
            const std::lock_guard lock(_mutex);
            _address = address;
        }

        [[nodiscard]] PeerAddress Get() const
        {
            const std::lock_guard lock(_mutex);
            return _address;
        }

    private:
        mutable std::mutex _mutex;
        PeerAddress _address;
    };

    struct RigOptions
    {
        /// With the access token (so a rate limiter), or a host with no authentication
        bool token = true;
        std::chrono::milliseconds hello = EditorServer::DefaultHelloTimeout;
        std::chrono::milliseconds idle = EditorServer::DefaultIdleTimeout;
        std::chrono::milliseconds write = EditorServer::DefaultWriteTimeout;
        std::chrono::milliseconds maxSession{0};
        /// A small send buffer on every accepted socket (with the client's small receive buffer, a response bigger than
        /// both cannot be written until the client reads)
        int sendBufferBytes = 0;
        std::optional<PeerAddress> peer;
    };

    /// A server whose peers, connection clock and rate limiter clock the test chooses
    struct Rig
    {
        explicit Rig(const RigOptions &options = RigOptions{})
        {
            if (options.peer.has_value())
            {
                peer.Set(*options.peer);
            }
            if (options.token)
            {
                EXPECT_TRUE(server.SetAccessToken(Token));
            }
            EXPECT_TRUE(server.SetHelloTimeout(options.hello));
            EXPECT_TRUE(server.SetIdleTimeout(options.idle));
            EXPECT_TRUE(server.SetWriteTimeout(options.write));
            EXPECT_TRUE(server.SetMaxSessionAge(options.maxSession));
            EXPECT_TRUE(server.SetConnectionClock([this] { return TimeAt(connectionMs.load()); }));
            EXPECT_TRUE(server.SetRateLimitClock([this] { return TimeAt(rateLimitMs.load()); }));
            EXPECT_TRUE(server.SetPeerAddressProvider([this](int) { return peer.Get(); }));
            if (options.sendBufferBytes > 0)
            {
                const int size = options.sendBufferBytes;
                EXPECT_TRUE(server.SetAcceptedSocketHook(
                    [size](int socket)
                    { N2TestSockets::SetSendBuffer(static_cast<N2TestSockets::Handle>(socket), size); }));
            }
            started = server.Start(0);
        }

        ~Rig() { StopWithin(server, 10s); }

        Rig(const Rig &) = delete;
        Rig &operator=(const Rig &) = delete;

        static std::chrono::steady_clock::time_point TimeAt(int64_t milliseconds)
        {
            return std::chrono::steady_clock::time_point(std::chrono::milliseconds(milliseconds));
        }

        void AdvanceConnection(std::chrono::milliseconds by) { connectionMs += by.count(); }
        void AdvanceRateLimit(std::chrono::milliseconds by) { rateLimitMs += by.count(); }

        /// Moves the connection clock on by `step` until the server has closed this connection (true), a step at a time,
        /// each followed by a short wait on the close: the server notices a deadline within one poll interval, and a
        /// step taken before it accepted the connection is simply made up by the next one. The step is larger than the
        /// deadline being tested, so one step past the server's reading of the clock is enough.
        bool AdvanceUntilClosed(TestClient &client, std::chrono::milliseconds step)
        {
            for (int attempt = 0; attempt < 300; ++attempt)
            {
                AdvanceConnection(step);
                if (client.IsClosedWithin(30))
                    return true;
            }
            return false;
        }

        std::optional<Frame> Hello(TestClient &client) { return Roundtrip(server, client, CommandType::Hello, HelloPayload(Token)); }

        static inline const PeerAddress Attacker = PeerAddress::IPv4(203, 0, 113, 7);
        static inline const PeerAddress Loopback = PeerAddress::IPv4(127, 0, 0, 1);

        // Before the server: it is stopped first, and its thread reads all of them
        SharedPeer peer{Attacker};
        std::atomic<int64_t> connectionMs{1000};
        std::atomic<int64_t> rateLimitMs{1000};
        EditorServer server;
        bool started = false;
    };

    /// A connection that gets as far as the wire and then ends some way other than a Hello
    enum class BreakIn
    {
        CloseAtOnce,
        HalfAHeader,
        MidPayload,
        CommandBeforeHello,
        OversizedFrame,
    };

    const char *Name(const BreakIn how)
    {
        switch (how)
        {
        case BreakIn::CloseAtOnce: return "CloseAtOnce";
        case BreakIn::HalfAHeader: return "HalfAHeader";
        case BreakIn::MidPayload: return "MidPayload";
        case BreakIn::CommandBeforeHello: return "CommandBeforeHello";
        case BreakIn::OversizedFrame: return "OversizedFrame";
        }
        return "?";
    }

    void ConnectAndBreakIn(Rig &rig, const BreakIn how)
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        switch (how)
        {
        case BreakIn::CloseAtOnce:
            break;
        case BreakIn::HalfAHeader:
        {
            const uint8_t type = static_cast<uint8_t>(CommandType::Hello);
            ASSERT_TRUE(N2TestSockets::SendAll(client.Socket(), &type, 1));
            break;
        }
        case BreakIn::MidPayload:
        {
            // A Hello that promises 200 bytes and sends 10
            BufferWriter w;
            w.WriteU8(static_cast<uint8_t>(CommandType::Hello));
            w.WriteU32(200);
            w.WriteBytes(std::vector<uint8_t>(10, 0));
            ASSERT_TRUE(N2TestSockets::SendAll(client.Socket(), w.Data().data(), w.Size()));
            break;
        }
        case BreakIn::CommandBeforeHello:
        {
            ASSERT_TRUE(client.SendRequest(CommandType::GetEngineHealth, {}));
            const auto response = client.ReadResponse();
            ASSERT_TRUE(response.has_value());
            EXPECT_EQ(Decode(*response).type, ErrorType);
            break;
        }
        case BreakIn::OversizedFrame:
        {
            // Declares more than a Hello needs and sends none of it: answered from the header alone
            BufferWriter w;
            w.WriteU8(static_cast<uint8_t>(CommandType::Hello));
            w.WriteU32(EditorServer::MaxPayloadBytesBeforeHello + 1);
            ASSERT_TRUE(N2TestSockets::SendAll(client.Socket(), w.Data().data(), w.Size()));
            const auto response = client.ReadResponse();
            ASSERT_TRUE(response.has_value());
            EXPECT_EQ(Decode(*response).type, ErrorType);
            break;
        }
        }
    }
}

// ==================== Before Hello: every connection that doesn't finish one is counted (F1) ====================

TEST(PreHelloAccountingTest, AnIdlePeerIsClosedAtTheDeadlineAndCountedUntilItsAddressIsBackedOff)
{
    LogCapture log;
    Rig rig;
    ASSERT_TRUE(rig.started);

    // The budget is 2 s of connection time: a step of 3 s passes it. Each connection is served, held and closed by the
    // server (nothing refuses it at accept) and is a failed Hello for its address.
    for (int i = 0; i < 3; ++i)
    {
        TestClient idle(rig.server.GetPort());
        ASSERT_TRUE(idle.IsConnected());
        ASSERT_TRUE(rig.AdvanceUntilClosed(idle, 3s)) << i;
    }
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u) << "none of the three was refused at accept";

    // The third failure started the backoff: the fourth connection is closed at accept, with the clock not moved (a
    // connection that was served would be held until its 2 s passed)
    {
        TestClient refused(rig.server.GetPort());
        ASSERT_TRUE(refused.IsConnected());
        EXPECT_TRUE(refused.IsClosedWithin(5000));
    }
    EXPECT_EQ(rig.server.RateLimitedConnections(), 1u);
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "Rate limit: peer=203.0.113.7 is backed off for 1000 ms after 3 failed Hellos"));
    EXPECT_EQ(log.Count("no Hello within 2000 ms"), 3u) << "each timeout logged its one line, with the peer";
    EXPECT_EQ(log.Count("no Hello within 2000 ms peer=203.0.113.7"), 3u);

    // The backoff ends with the rate limiter's clock, and a real client gets in
    rig.AdvanceRateLimit(1000ms);
    TestClient good(rig.server.GetPort());
    ASSERT_TRUE(good.IsConnected());
    const auto hello = rig.Hello(good);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);
}

TEST(PreHelloAccountingTest, EveryWayOutWithoutAHelloCountsAsAFailure)
{
    for (const BreakIn how : {BreakIn::CloseAtOnce, BreakIn::HalfAHeader, BreakIn::MidPayload,
                              BreakIn::CommandBeforeHello, BreakIn::OversizedFrame})
    {
        SCOPED_TRACE(Name(how));
        Rig rig;
        ASSERT_TRUE(rig.started);

        for (int i = 0; i < 3; ++i)
        {
            ConnectAndBreakIn(rig, how);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());
        }

        // The server serves one connection at a time, so the three have all been counted before it accepts this one
        TestClient next(rig.server.GetPort());
        ASSERT_TRUE(next.IsConnected());
        EXPECT_TRUE(next.IsClosedWithin(5000)) << "the address is backed off";
        EXPECT_EQ(rig.server.RateLimitedConnections(), 1u);
    }
}

TEST(PreHelloAccountingTest, AWrongTokenHelloIsCountedOnceNotTwice)
{
    // Refused for its token, the Hello is counted when it is refused; the connection that closes after it must not be
    // counted again, or two wrong tokens would ban an address that is allowed two
    Rig rig;
    ASSERT_TRUE(rig.started);
    for (int i = 0; i < 2; ++i)
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto response = Roundtrip(rig.server, client, CommandType::Hello, HelloPayload("wrong-token"));
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->type, ErrorType);
    }
    TestClient good(rig.server.GetPort());
    ASSERT_TRUE(good.IsConnected());
    const auto hello = rig.Hello(good);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);
}

TEST(PreHelloAccountingTest, ASuccessfulHelloIsNotAFailureWhateverHappensAfterIt)
{
    Rig rig;
    ASSERT_TRUE(rig.started);
    for (int i = 0; i < 6; ++i)
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = rig.Hello(client);
        ASSERT_TRUE(hello.has_value()) << i;
        EXPECT_EQ(hello->type, ServerInfoType) << i;
        // closes without a word, as a client does
    }
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);
}

TEST(PreHelloAccountingTest, LoopbackIdleConnectionsAreNeitherBannedNorFeedTheGlobalBackoff)
{
    LogCapture log;
    RigOptions options;
    options.peer = Rig::Loopback;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    // Past what one address may do (3), and past loopback's whole-of-loopback burst (10): behind a tunnel or a proxy
    // every peer is loopback, and counting a mere connect would make the lockout cheap
    for (int i = 0; i < 12; ++i)
    {
        TestClient idle(rig.server.GetPort());
        ASSERT_TRUE(idle.IsConnected());
        ASSERT_TRUE(rig.AdvanceUntilClosed(idle, 3s)) << i;
    }
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);

    // Neither this loopback address nor another is refused, and a real client gets in
    for (const PeerAddress &address : {Rig::Loopback, PeerAddress::IPv4(127, 3, 3, 3)})
    {
        rig.peer.Set(address);
        TestClient good(rig.server.GetPort());
        ASSERT_TRUE(good.IsConnected());
        const auto hello = rig.Hello(good);
        ASSERT_TRUE(hello.has_value()) << address.ToString();
        EXPECT_EQ(hello->type, ServerInfoType) << address.ToString();
    }
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);
    EXPECT_FALSE(log.Has("Rate limit:")) << "no backoff, per address or for loopback, was started";
}

TEST(PreHelloAccountingTest, AnAddressOnRecordStillGetsTheFullBudget)
{
    // The same address, once its backoff has run, still has the 2 s budget (the prior-offender one is no shorter)
    Rig rig;
    ASSERT_TRUE(rig.started);
    for (int i = 0; i < 3; ++i)
    {
        TestClient idle(rig.server.GetPort());
        ASSERT_TRUE(idle.IsConnected());
        ASSERT_TRUE(rig.AdvanceUntilClosed(idle, 3s)) << i;
    }
    rig.AdvanceRateLimit(1000ms);

    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    // 1.5 s of connection time passes before the Hello: inside a 2 s budget, which is still the budget
    rig.AdvanceConnection(1500ms);
    const auto hello = rig.Hello(client);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType);
}

TEST(PreHelloAccountingTest, ThePreHelloBudgetIsTwoSecondsAndAHostWithoutATokenCountsNothing)
{
    EXPECT_EQ(EditorServer::DefaultHelloTimeout, 2000ms);
    EditorServer probe;
    EXPECT_EQ(probe.GetHelloTimeout(), 2000ms);

    RigOptions options;
    options.token = false;
    options.idle = 0ms; // not what is being tested
    Rig rig(options);
    ASSERT_TRUE(rig.started);
    EXPECT_FALSE(rig.server.IsRateLimiting());

    // Idle past the Hello budget, then a command runs: with no token there is no Hello to be late with
    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    rig.AdvanceConnection(60s);
    const auto response = Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload());
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);
}

// ==================== After Hello: idle, write and session deadlines (F2) ====================

TEST(ConnectionDeadlineTest, AnAuthenticatedIdleClientIsDisconnectedAtTheIdleDeadlineWithOneLine)
{
    LogCapture log;
    Rig rig;
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = rig.Hello(client);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType);

    // The default is 5 minutes; a step of 301 s passes it
    ASSERT_TRUE(rig.AdvanceUntilClosed(client, 301s));
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "no request for 300000 ms (the idle timeout) peer=203.0.113.7"));
    EXPECT_EQ(log.Count("the idle timeout"), 1u) << "one line per close";

    // The server is free: another client gets in at once (and the idle one wasn't a failed Hello)
    TestClient next(rig.server.GetPort());
    ASSERT_TRUE(next.IsConnected());
    const auto again = rig.Hello(next);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->type, ServerInfoType);
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);
    for (const std::string &line : log.Lines())
    {
        EXPECT_EQ(line.find(Token), std::string::npos) << line;
    }
}

TEST(ConnectionDeadlineTest, AClientThatKeepsPollingStaysConnectedForLongerThanTheIdleTimeout)
{
    LogCapture log;
    Rig rig;
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = rig.Hello(client);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType);

    // Twenty minutes of connection time, a request a minute: every request restarts the 5 minutes, polled ones included
    for (int minute = 0; minute < 20; ++minute)
    {
        rig.AdvanceConnection(60s);
        const auto response = Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload());
        ASSERT_TRUE(response.has_value()) << "minute " << minute;
    }
    EXPECT_FALSE(log.Has("the idle timeout"));
    EXPECT_TRUE(rig.server.IsRunning());
}

TEST(ConnectionDeadlineTest, AHostWithoutATokenClosesAnIdleSessionAndHasNoLimiter)
{
    LogCapture log;
    RigOptions options;
    options.token = false;
    Rig rig(options);
    ASSERT_TRUE(rig.started);
    EXPECT_FALSE(rig.server.IsRateLimiting());

    // A session from accept: it says nothing, and is closed at the idle deadline
    TestClient idle(rig.server.GetPort());
    ASSERT_TRUE(idle.IsConnected());
    ASSERT_TRUE(rig.AdvanceUntilClosed(idle, 301s));
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "no request for 300000 ms (the idle timeout) peer=203.0.113.7"));

    TestClient next(rig.server.GetPort());
    ASSERT_TRUE(next.IsConnected());
    const auto response = Roundtrip(rig.server, next, CommandType::Hello, HelloPayload("any token"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ServerInfoType);
    EXPECT_EQ(rig.server.RateLimitedConnections(), 0u);
}

TEST(ConnectionDeadlineTest, ZeroTurnsTheIdleTimeoutOffAndAnythingIsPossibleForever)
{
    RigOptions options;
    options.token = false;
    options.idle = 0ms;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    ASSERT_TRUE(Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload()).has_value());
    rig.AdvanceConnection(std::chrono::hours(24 * 7));
    ASSERT_TRUE(Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload()).has_value());
    EXPECT_FALSE(client.IsClosedWithin(50));
}

TEST(ConnectionDeadlineTest, TheMaxSessionAgeClosesASessionWhenSetAndNeverWhenOff)
{
    LogCapture log;
    {
        RigOptions options;
        options.token = false;
        options.idle = 0ms;
        options.maxSession = 10min;
        Rig rig(options);
        ASSERT_TRUE(rig.started);

        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        ASSERT_TRUE(Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload()).has_value());
        rig.AdvanceConnection(9min);
        ASSERT_TRUE(Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload()).has_value())
            << "9 minutes into a 10 minute limit";

        // Busy, not idle: the age counts anyway
        ASSERT_TRUE(rig.AdvanceUntilClosed(client, 2min));
        ASSERT_TRUE(PumpUntilLogged(rig.server, log, "its session reached the maximum age of 600000 ms peer=203.0.113.7"));
        EXPECT_EQ(log.Count("maximum age"), 1u);
    }
    {
        RigOptions options;
        options.token = false;
        options.idle = 0ms;
        Rig rig(options); // maxSession is 0: off
        ASSERT_TRUE(rig.started);
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        ASSERT_TRUE(Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload()).has_value());
        rig.AdvanceConnection(std::chrono::hours(24 * 365));
        ASSERT_TRUE(Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload()).has_value());
    }
}

TEST(ConnectionDeadlineTest, AnotherHelloDoesNotRestartTheSessionAge)
{
    // With a token the age counts from the first good Hello; saying Hello again must not buy a session more time
    LogCapture log;
    RigOptions options;
    options.idle = 0ms;
    options.maxSession = 10min;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto first = rig.Hello(client);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->type, ServerInfoType);
    rig.AdvanceConnection(6min);
    const auto again = rig.Hello(client);
    ASSERT_TRUE(again.has_value());
    ASSERT_EQ(again->type, ServerInfoType);

    // 12 minutes after the first Hello, 6 after the second
    ASSERT_TRUE(rig.AdvanceUntilClosed(client, 6min));
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "its session reached the maximum age"));
}

TEST(ConnectionDeadlineTest, AClientThatNeverReadsCantBlockTheServerPastTheWriteDeadline)
{
    LogCapture log;
    RigOptions options;
    options.token = false; // a session from accept
    options.idle = 0ms;    // only the write deadline is being tested
    options.sendBufferBytes = 4096;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort(), 4096);
    ASSERT_TRUE(client.IsConnected());

    // CreateScript answers with a script template that holds the name about ten times: a 1 MiB name asks for a ~10 MiB
    // response, far more than the two small buffers hold. The client never reads it.
    BufferWriter name;
    name.WriteString(std::string(1024 * 1024, 'n'));
    ASSERT_TRUE(client.SendRequest(CommandType::CreateScript, name.Release()));

    // The main loop runs the command first
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "Generated script template")) << DumpLines(log);

    // The network thread then blocks writing the response. Time passes in steps longer than the write timeout until the
    // server gives up (it logs once).
    const auto limit = std::chrono::steady_clock::now() + 10s;
    while (!log.Has("no write progress") && std::chrono::steady_clock::now() < limit)
    {
        rig.AdvanceConnection(31s);
        rig.server.ProcessCommands(10ms);
    }
    ASSERT_TRUE(log.Has("no write progress for 30000 ms (the write timeout), the client isn't reading peer=203.0.113.7"))
        << "bytes waiting at the client: " << client.BytesAvailable() << "
" << DumpLines(log);
    EXPECT_EQ(log.Count("no write progress"), 1u);

    // What the client can still read is cut short, and then the connection is closed
    std::vector<uint8_t> buffer(65536);
    long long total = 0;
    long long last = 0;
    while ((last = client.ReadSome(buffer.data(), buffer.size())) > 0)
    {
        total += last;
    }
    EXPECT_TRUE(last == 0 || !N2TestSockets::LastErrorWasTimeout()) << "the server closed the connection";
    EXPECT_LT(total, 5 * 1024 * 1024) << "not the whole response";

    // The server is free: a second client connects and gets ServerInfo
    TestClient next(rig.server.GetPort());
    ASSERT_TRUE(next.IsConnected());
    const auto response = Roundtrip(rig.server, next, CommandType::Hello, HelloPayload(""));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->type, ServerInfoType);
}

TEST(ConnectionDeadlineTest, ASlowClientThatKeepsReadingALargeFrameIsNotCutOff)
{
    LogCapture log;
    RigOptions options;
    options.token = false;
    options.idle = 0ms;
    options.write = 2s;
    options.sendBufferBytes = 4096;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort(), 4096);
    ASSERT_TRUE(client.IsConnected());

    // A ~440 KB response, read in 4 KiB slices with 100 ms of connection time between them: ten seconds in all, five
    // times the write timeout, but the client never stops reading for 2 s, so progress keeps restarting it
    BufferWriter name;
    name.WriteString(std::string(44 * 1024, 'n'));
    ASSERT_TRUE(client.SendRequest(CommandType::CreateScript, name.Release()));
    const auto limit = std::chrono::steady_clock::now() + 10s;
    while (client.BytesAvailable() < 5 && std::chrono::steady_clock::now() < limit)
    {
        rig.server.ProcessCommands(10ms);
    }
    uint8_t header[5];
    size_t headerRead = 0;
    while (headerRead < sizeof(header))
    {
        const long long got = client.ReadSome(header + headerRead, sizeof(header) - headerRead);
        ASSERT_GT(got, 0);
        headerRead += static_cast<size_t>(got);
    }
    uint32_t size = 0;
    std::memcpy(&size, header + 1, sizeof(size));
    ASSERT_GT(size, 400u * 1024u);

    std::vector<uint8_t> buffer(4096);
    size_t total = 0;
    while (total < size)
    {
        const size_t want = std::min<size_t>(buffer.size(), size - total);
        const long long got = client.ReadSome(buffer.data(), want);
        ASSERT_GT(got, 0) << "cut off after " << total << " of " << size << " bytes";
        total += static_cast<size_t>(got);
        rig.AdvanceConnection(100ms);
    }
    EXPECT_EQ(total, size);

    // Still connected: the next request on the same connection is answered, and no deadline line was written
    const auto response = Roundtrip(rig.server, client, CommandType::PollEvents, PollPayload());
    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(log.Has("no write progress"));
}

TEST(ConnectionDeadlineTest, TheSettingsHaveLimitsAndCantChangeWhileServing)
{
    EditorServer probe;
    EXPECT_EQ(probe.GetHelloTimeout(), 2000ms);
    EXPECT_EQ(probe.GetIdleTimeout(), 5min);
    EXPECT_EQ(probe.GetWriteTimeout(), 30s);
    EXPECT_EQ(probe.GetMaxSessionAge(), 0ms) << "no maximum session age by default";

    EXPECT_FALSE(probe.SetIdleTimeout(-1ms));
    EXPECT_TRUE(probe.SetIdleTimeout(0ms)) << "zero is off";
    EXPECT_EQ(probe.GetIdleTimeout(), 0ms);
    EXPECT_FALSE(probe.SetWriteTimeout(0ms)) << "a write must always have a deadline";
    EXPECT_FALSE(probe.SetWriteTimeout(-1ms));
    EXPECT_TRUE(probe.SetWriteTimeout(1ms));
    EXPECT_FALSE(probe.SetMaxSessionAge(-1ms));
    EXPECT_TRUE(probe.SetMaxSessionAge(0ms)) << "zero is off";

    Rig rig;
    ASSERT_TRUE(rig.started);
    EXPECT_FALSE(rig.server.SetIdleTimeout(1s));
    EXPECT_FALSE(rig.server.SetWriteTimeout(1s));
    EXPECT_FALSE(rig.server.SetMaxSessionAge(1s));
    EXPECT_FALSE(rig.server.SetConnectionClock({}));
    EXPECT_FALSE(rig.server.SetAcceptedSocketHook({}));
    EXPECT_EQ(rig.server.GetIdleTimeout(), EditorServer::DefaultIdleTimeout);
    EXPECT_EQ(rig.server.GetWriteTimeout(), EditorServer::DefaultWriteTimeout);
    EXPECT_EQ(rig.server.GetMaxSessionAge(), 0ms);
}
