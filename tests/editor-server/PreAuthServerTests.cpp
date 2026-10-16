#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
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
#include <editor-server/TokenBucket.hpp>
#include <engine/Logger.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;

// The structure of the editor host's network side (PR S-3): an acceptor, N pre-auth workers and one session thread.
// Real sockets over loopback; the peer address comes from an injected provider, so one machine plays many peers.
// Time is real here (the budgets are seconds long and the tests stay far inside them), except for the handshake rate
// bucket, whose clock the test moves. The pure classes (slots, bucket, arbiter, limiter) have their own tests.

namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr const char *Token = "pre-auth-token";

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return std::string(payload.begin(), payload.end()); }
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

    std::vector<uint8_t> HelloPayload(const std::string &token, const bool errorCodes = false)
    {
        BufferWriter w;
        w.WriteString("pre-auth test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        if (errorCodes)
        {
            w.WriteBool(true);
        }
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
        explicit TestClient(int port)
        {
            _socket = N2TestSockets::OpenTcp();
            if (_socket == N2TestSockets::InvalidHandle)
                return;
            N2TestSockets::SetReceiveTimeout(_socket, 10000); // a hung server fails the test instead of blocking it
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

        bool SendRequest(CommandType type, const std::vector<uint8_t> &payload)
        {
            BufferWriter w;
            w.WriteU8(static_cast<uint8_t>(type));
            w.WriteU32(static_cast<uint32_t>(payload.size()));
            w.WriteBytes(payload);
            return N2TestSockets::SendAll(_socket, w.Data().data(), w.Size());
        }

        [[nodiscard]] size_t BytesAvailable() const { return N2TestSockets::BytesAvailable(_socket); }

        /// The next whole frame, bytes as the server sent them; nullopt when the connection closed or timed out first
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

    /// Sends a request and plays the host's main loop until its response arrives; the response's raw bytes
    std::optional<std::vector<uint8_t>> RoundtripRaw(EditorServer &server, TestClient &client, const CommandType type,
                                                     const std::vector<uint8_t> &payload = {})
    {
        if (!client.SendRequest(type, payload))
            return std::nullopt;
        const auto limit = std::chrono::steady_clock::now() + 10s;
        while (client.BytesAvailable() < 5 && std::chrono::steady_clock::now() < limit)
        {
            server.ProcessCommands(10ms);
        }
        return client.ReadResponse();
    }

    std::optional<Frame> Roundtrip(EditorServer &server, TestClient &client, const CommandType type,
                                   const std::vector<uint8_t> &payload = {})
    {
        const auto raw = RoundtripRaw(server, client, type, payload);
        if (!raw.has_value())
            return std::nullopt;
        return Decode(*raw);
    }

    /// Plays the host's main loop for a while (the network threads post their log lines to it)
    void Pump(EditorServer &server, std::chrono::milliseconds duration)
    {
        const auto end = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < end)
        {
            server.ProcessCommands(10ms);
        }
    }

    /// Plays the host's main loop until the log has this text
    bool PumpUntilLogged(EditorServer &server, const LogCapture &log, const std::string_view text)
    {
        const auto limit = std::chrono::steady_clock::now() + 10s;
        while (!log.Has(text) && std::chrono::steady_clock::now() < limit)
        {
            server.ProcessCommands(10ms);
        }
        return log.Has(text);
    }

    /// Waits (up to a limit) for a condition the server's threads make true
    template <typename Condition>
    bool WaitFor(Condition condition, std::chrono::milliseconds limit = 10s)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= end)
                return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
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
            std::fprintf(stderr, "EditorServer::Stop() hung\n");
            std::abort(); // a hung thread can't be joined
        }
        stopper.join();
    }

    /// The address the provider hands the acceptor: the test sets it before each connection
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

    PeerAddress Remote(const uint8_t last) { return PeerAddress::IPv4(203, 0, 113, last); }
    const PeerAddress Loopback = PeerAddress::IPv4(127, 0, 0, 1);

    struct RigOptions
    {
        /// With the access token (a Hello), or a host with no authentication
        bool token = true;
        size_t slots = 4;
        /// Long, so a silent connection is still held when a test looks at it
        std::chrono::milliseconds hello = 20s;
        /// Long enough that a client that connects as the last one leaves is never turned away, even on a slow runner
        std::chrono::milliseconds busyGrace = 3s;
        std::optional<TokenBucketOptions> bucket;
        TokenBucket::Clock bucketClock;
        std::chrono::milliseconds summaryInterval = EditorServer::DefaultAdmissionSummaryInterval;
        bool readOnly = false;
    };

    /// A server whose peers the test chooses
    struct Rig
    {
        explicit Rig(const RigOptions &options = RigOptions{})
        {
            if (options.token)
            {
                EXPECT_TRUE(server.SetAccessToken(Token));
            }
            EXPECT_TRUE(server.SetHelloTimeout(options.hello));
            EXPECT_TRUE(server.SetPreAuthSlots(options.slots));
            EXPECT_TRUE(server.SetBusyGrace(options.busyGrace));
            EXPECT_TRUE(server.SetAdmissionSummaryInterval(options.summaryInterval));
            EXPECT_TRUE(server.SetHandshakeRateLimit(options.bucket, options.bucketClock));
            EXPECT_TRUE(server.SetPeerAddressProvider([this](int) { return peer.Get(); }));
            if (options.readOnly)
            {
                EXPECT_TRUE(server.SetReadOnly(true));
            }
            started = server.Start(0);
        }

        ~Rig() { StopWithin(server, 10s); }

        Rig(const Rig &) = delete;
        Rig &operator=(const Rig &) = delete;

        /// Connects as this address and waits until the acceptor has dealt with the connection: it holds a slot, or it
        /// was refused (so the next call may change the address)
        std::unique_ptr<TestClient> ConnectSilent(const PeerAddress &address)
        {
            const uint64_t before = server.PreAuthRefusedConnections() + server.RateLimitedConnections();
            const size_t slotsBefore = server.PreAuthSlotsInUse();
            peer.Set(address);
            auto client = std::make_unique<TestClient>(server.GetPort());
            EXPECT_TRUE(client->IsConnected());
            EXPECT_TRUE(WaitFor(
                [&]
                {
                    return server.PreAuthSlotsInUse() > slotsBefore ||
                           server.PreAuthRefusedConnections() + server.RateLimitedConnections() > before;
                }));
            return client;
        }

        /// A client from this address that says Hello with the token
        std::optional<Frame> Hello(TestClient &client, const bool errorCodes = false)
        {
            return Roundtrip(server, client, CommandType::Hello, HelloPayload(Token, errorCodes));
        }

        // Before the server: it is stopped first, and its threads read the peer
        SharedPeer peer{Loopback};
        EditorServer server;
        bool started = false;
    };
}

// ==================== The point of the stage: a stalled peer no longer starves the host ====================

TEST(PreAuthServerTest, SilentConnectionsInTheOtherSlotsDoNotBlockAnAuthenticatedClient)
{
    LogCapture log;
    Rig rig;
    ASSERT_TRUE(rig.started);

    // Three of the four slots are held by peers that connect and say nothing (slow loris), each from its own address
    std::vector<std::unique_ptr<TestClient>> silent;
    for (uint8_t i = 0; i < 3; ++i)
    {
        silent.push_back(rig.ConnectSilent(Remote(static_cast<uint8_t>(10 + i))));
    }
    ASSERT_EQ(rig.server.PreAuthSlotsInUse(), 3u);

    // A client with the token, from a fourth address, is served at once: before this stage it would have waited behind
    // every one of them for the Hello timeout
    rig.peer.Set(Remote(50));
    TestClient good(rig.server.GetPort());
    ASSERT_TRUE(good.IsConnected());
    const auto hello = rig.Hello(good);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    const auto read = Roundtrip(rig.server, good, CommandType::GetPlayState);
    ASSERT_TRUE(read.has_value()) << "the session works while three slots are held";

    // The silent ones are still held: nothing closed them to make room
    for (const auto &client : silent)
    {
        EXPECT_FALSE(client->IsClosedWithin(20));
    }
    EXPECT_EQ(rig.server.BusyRefusals(), 0u);
    EXPECT_EQ(rig.server.SessionHandoffs(), 1u);
}

TEST(PreAuthServerTest, AllSlotsHeldByStalledPeersStillLeaveTheSessionAlone)
{
    // The other half of G1: an authenticated session that is already running is not touched by a full house
    Rig rig;
    ASSERT_TRUE(rig.started);

    TestClient session(rig.server.GetPort());
    ASSERT_TRUE(session.IsConnected());
    const auto hello = rig.Hello(session);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

    std::vector<std::unique_ptr<TestClient>> silent;
    for (uint8_t i = 0; i < 4; ++i)
    {
        silent.push_back(rig.ConnectSilent(Remote(static_cast<uint8_t>(20 + i))));
    }
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 4u);
    for (int i = 0; i < 10; ++i)
    {
        const auto response = Roundtrip(rig.server, session, CommandType::GetPlayState);
        ASSERT_TRUE(response.has_value()) << i;
    }
}

TEST(PreAuthServerTest, ASessionKeepsAnsweringWhileConnectionsComeAndGo)
{
    Rig rig;
    ASSERT_TRUE(rig.started);

    TestClient session(rig.server.GetPort());
    ASSERT_TRUE(session.IsConnected());
    const auto hello = rig.Hello(session);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

    // 100 connect-and-close cycles (some find every slot busy and are closed at accept; some get a slot and are closed
    // by their peer), with the session's round trips between them
    for (int i = 0; i < 100; ++i)
    {
        {
            TestClient visitor(rig.server.GetPort());
            ASSERT_TRUE(visitor.IsConnected());
        }
        if (i % 5 == 0)
        {
            const auto response = Roundtrip(rig.server, session, CommandType::GetPlayState);
            ASSERT_TRUE(response.has_value()) << "round trip " << i;
        }
    }
    const auto last = Roundtrip(rig.server, session, CommandType::GetPlayState);
    EXPECT_TRUE(last.has_value());
}

// ==================== Slot admission ====================

TEST(PreAuthServerTest, TheConnectionPastTheLastSlotIsClosedAtOnceWithoutALine)
{
    LogCapture log;
    RigOptions options;
    options.slots = 2;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    // Loopback may use every slot (behind a proxy everyone is loopback)
    auto first = rig.ConnectSilent(Loopback);
    auto second = rig.ConnectSilent(Loopback);
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 2u);
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 0u);

    auto third = rig.ConnectSilent(Loopback);
    EXPECT_TRUE(third->IsClosedWithin(5000)) << "no slot free: closed at once, no TLS, no Hello wait";
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 1u);
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 2u);
    EXPECT_FALSE(first->IsClosedWithin(20));
    EXPECT_FALSE(second->IsClosedWithin(20));

    // Pre-auth connections write nothing per connection: no "connected" line, no peer in any line
    Pump(rig.server, 200ms);
    EXPECT_EQ(log.Count("Editor connected"), 0u);
    EXPECT_EQ(log.Count("peer=127.0.0.1"), 0u);
}

TEST(PreAuthServerTest, ASlotIsFreedWhenItsConnectionEndsOnEveryPath)
{
    RigOptions options;
    options.slots = 1;
    options.hello = 400ms; // short: the timeout path ends within the test
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    // 1. closed by the peer
    {
        auto client = rig.ConnectSilent(Loopback);
        ASSERT_EQ(rig.server.PreAuthSlotsInUse(), 1u);
    }
    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; }));

    // 2. the Hello budget runs out
    {
        auto client = rig.ConnectSilent(Loopback);
        EXPECT_TRUE(client->IsClosedWithin(5000));
    }
    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; }));

    // 3. a command other than Hello is refused
    {
        auto client = rig.ConnectSilent(Loopback);
        const auto response = Roundtrip(rig.server, *client, CommandType::GetPlayState);
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->type, ErrorType);
        EXPECT_TRUE(client->IsClosedWithin(5000));
    }
    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; }));

    // 4. a wrong token
    {
        auto client = rig.ConnectSilent(Loopback);
        const auto response = Roundtrip(rig.server, *client, CommandType::Hello, HelloPayload("wrong"));
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->type, ErrorType);
        EXPECT_EQ(response->Text(), "Invalid access token");
    }
    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; }));

    // 5. a good Hello: the connection becomes the session and its slot is free at once
    TestClient good(rig.server.GetPort());
    ASSERT_TRUE(good.IsConnected());
    const auto hello = rig.Hello(good);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; }));

    // ... so the one slot serves the next stalled peer, not a leaked count
    auto again = rig.ConnectSilent(Loopback);
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 1u);
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 0u) << "no connection was refused for a slot that leaked";
}

TEST(PreAuthServerTest, ANonLoopbackAddressGetsTwoSlotsAndTheRestAreClosedAtOnce)
{
    LogCapture log;
    Rig rig;
    ASSERT_TRUE(rig.started);

    std::vector<std::unique_ptr<TestClient>> attacker;
    for (int i = 0; i < 5; ++i)
    {
        attacker.push_back(rig.ConnectSilent(Remote(7)));
    }
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 2u) << "two for one address";
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 3u);
    EXPECT_FALSE(attacker[0]->IsClosedWithin(20));
    EXPECT_FALSE(attacker[1]->IsClosedWithin(20));
    for (size_t i = 2; i < attacker.size(); ++i)
    {
        EXPECT_TRUE(attacker[i]->IsClosedWithin(5000)) << i;
    }

    // Another address still gets in, and is served
    rig.peer.Set(Remote(8));
    TestClient other(rig.server.GetPort());
    ASSERT_TRUE(other.IsConnected());
    const auto hello = rig.Hello(other);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
}

TEST(PreAuthServerTest, LoopbackMayUseEverySlot)
{
    Rig rig;
    ASSERT_TRUE(rig.started);

    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 4; ++i)
    {
        clients.push_back(rig.ConnectSilent(Loopback));
    }
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 4u);
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 0u);

    auto fifth = rig.ConnectSilent(Loopback);
    EXPECT_TRUE(fifth->IsClosedWithin(5000));
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 1u);
}

TEST(PreAuthServerTest, RefusedConnectionsAreCountedInOneSummaryLine)
{
    LogCapture log;
    RigOptions options;
    options.slots = 1;
    options.summaryInterval = 200ms;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    auto held = rig.ConnectSilent(Loopback);
    Pump(rig.server, 500ms);
    EXPECT_EQ(log.Count("Pre-auth limits: closed"), 0u) << "nothing refused, nothing reported";

    std::vector<std::unique_ptr<TestClient>> refused;
    for (int i = 0; i < 3; ++i)
    {
        refused.push_back(rig.ConnectSilent(Loopback));
    }
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 3u);
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "Pre-auth limits: closed"));
    EXPECT_TRUE(log.Has("connection(s) at accept without a word"));
    EXPECT_TRUE(log.Has("with every slot taken"));
}

// ==================== The handshake rate ====================

TEST(PreAuthServerTest, ConnectionsOverTheHandshakeRateAreClosedBeforeAnythingIsRead)
{
    // Two handshakes a second in all, on a clock the test moves
    std::atomic<int64_t> bucketMs{1000};
    RigOptions options;
    TokenBucketOptions bucket;
    bucket.globalPerSecond = 2;
    bucket.globalBurst = 2;
    bucket.perAddressPerSecond = 100;
    options.bucket = bucket;
    options.bucketClock = [&bucketMs] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(bucketMs.load())); };
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    for (int i = 0; i < 2; ++i)
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = rig.Hello(client);
        ASSERT_TRUE(hello.has_value()) << i;
        EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    }
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 0u);

    // The bucket is empty and the clock has not moved: the next two are closed at accept, before TLS and before a read
    for (int i = 0; i < 2; ++i)
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        EXPECT_TRUE(client.IsClosedWithin(5000)) << i;
    }
    EXPECT_EQ(rig.server.PreAuthRefusedConnections(), 2u);
    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; })) << "a refusal for the rate gives its slot back";

    // A second passes: two tokens are back, and a Hello is served again
    bucketMs += 1000;
    TestClient later(rig.server.GetPort());
    ASSERT_TRUE(later.IsConnected());
    const auto hello = rig.Hello(later);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
}

// ==================== One session at a time ====================

TEST(PreAuthServerTest, ASecondAuthenticatedClientGetsAPlainErrorWhileASessionIsActive)
{
    LogCapture log;
    RigOptions options;
    options.busyGrace = 300ms; // the wait for a session that is not ending
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient first(rig.server.GetPort());
    ASSERT_TRUE(first.IsConnected());
    const auto firstHello = rig.Hello(first);
    ASSERT_TRUE(firstHello.has_value());
    ASSERT_EQ(firstHello->type, ServerInfoType) << firstHello->Text();

    // The same token again, from another connection: Busy (a plain Error, no preemption, no takeover). The first
    // session is untouched.
    TestClient second(rig.server.GetPort());
    ASSERT_TRUE(second.IsConnected());
    const auto busy = rig.Hello(second);
    ASSERT_TRUE(busy.has_value());
    EXPECT_EQ(busy->type, ErrorType);
    EXPECT_EQ(busy->Text(), EditorServer::BusyError);
    EXPECT_TRUE(second.IsClosedWithin(5000));
    EXPECT_EQ(rig.server.BusyRefusals(), 1u);
    EXPECT_TRUE(PumpUntilLogged(rig.server, log, "Turned away an authenticated client"));

    const auto stillThere = Roundtrip(rig.server, first, CommandType::GetPlayState);
    ASSERT_TRUE(stillThere.has_value()) << "the first session was not closed for the second";
    EXPECT_NE(stillThere->type, ErrorType) << stillThere->Text();
}

TEST(PreAuthServerTest, TheNextClientIsServedOnceTheSessionHasEnded)
{
    RigOptions options;
    options.busyGrace = 5s; // the session ends within a moment of its peer closing; the grace covers it
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    for (int i = 0; i < 5; ++i)
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = rig.Hello(client);
        ASSERT_TRUE(hello.has_value()) << i;
        EXPECT_EQ(hello->type, ServerInfoType) << i << ": " << hello->Text();
    }
    EXPECT_EQ(rig.server.BusyRefusals(), 0u) << "a client that connects as the last one leaves is not turned away";
    EXPECT_EQ(rig.server.SessionHandoffs(), 5u);
}

TEST(PreAuthServerTest, AHostWithoutATokenStillServesOneConnectionAtATime)
{
    RigOptions options;
    options.token = false;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    auto first = std::make_unique<TestClient>(rig.server.GetPort());
    ASSERT_TRUE(first->IsConnected());
    const auto answered = Roundtrip(rig.server, *first, CommandType::GetPlayState);
    ASSERT_TRUE(answered.has_value());

    // The second connects (the kernel accepts it, a worker holds it) and sends a command: it waits, as it did in the
    // backlog. No Busy on a host without a token.
    TestClient second(rig.server.GetPort());
    ASSERT_TRUE(second.IsConnected());
    ASSERT_TRUE(second.SendRequest(CommandType::GetPlayState, {}));
    Pump(rig.server, 500ms);
    EXPECT_EQ(second.BytesAvailable(), 0u) << "served only after the first connection ends";
    EXPECT_EQ(rig.server.BusyRefusals(), 0u);

    first.reset();
    const auto limit = std::chrono::steady_clock::now() + 10s;
    while (second.BytesAvailable() < 5 && std::chrono::steady_clock::now() < limit)
    {
        rig.server.ProcessCommands(10ms);
    }
    const auto response = second.ReadResponse();
    ASSERT_TRUE(response.has_value());
    EXPECT_NE(Decode(*response).type, ErrorType) << Decode(*response).Text();
}

// ==================== The Hello replay keeps HandleHello's behaviour ====================

TEST(PreAuthServerTest, TheReplayedHelloGivesTheSameServerInfoAsTheOrdinaryPath)
{
    RigOptions options;
    options.readOnly = true; // a refused command is then the one place ErrorCoded is visible
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    const std::vector<uint8_t> payload = HelloPayload(Token, true);
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto viaSocket = RoundtripRaw(rig.server, client, CommandType::Hello, payload);
        ASSERT_TRUE(viaSocket.has_value());
        ASSERT_EQ(Decode(*viaSocket).type, ServerInfoType) << Decode(*viaSocket).Text();

        // The ordinary path: the same Hello run directly on the main thread (this thread plays it). ServerInfo is the
        // same bytes: protocol, engine version, capabilities, project, read-only, user and role.
        const std::vector<uint8_t> direct = rig.server.ExecuteCommand(static_cast<uint8_t>(CommandType::Hello), payload);
        EXPECT_EQ(*viaSocket, direct);

        // The Hello's error codes capability belongs to the connection: a refused command comes back as ErrorCoded
        const auto refused = Roundtrip(rig.server, client, CommandType::DestroyEntity,
                                       std::vector<uint8_t>{0, 0, 0, 0});
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorCodedType) << refused->Text();
    }
    {
        // A client that did not ask for codes gets the plain Error: the replay did not leak the last one's setting
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = rig.Hello(client, false);
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
        const auto refused = Roundtrip(rig.server, client, CommandType::DestroyEntity,
                                       std::vector<uint8_t>{0, 0, 0, 0});
        ASSERT_TRUE(refused.has_value());
        EXPECT_EQ(refused->type, ErrorType) << refused->Text();
    }
}

TEST(PreAuthServerTest, ARefusedHelloIsAnsweredOnTheWorkerAndNeverReachesTheSession)
{
    LogCapture log;
    Rig rig;
    ASSERT_TRUE(rig.started);

    TestClient wrong(rig.server.GetPort());
    ASSERT_TRUE(wrong.IsConnected());
    const auto refused = Roundtrip(rig.server, wrong, CommandType::Hello, HelloPayload("not-the-token"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->type, ErrorType);
    EXPECT_EQ(refused->Text(), "Invalid access token");
    EXPECT_TRUE(wrong.IsClosedWithin(5000));

    TestClient old(rig.server.GetPort());
    ASSERT_TRUE(old.IsConnected());
    BufferWriter w;
    w.WriteString("pre-auth test");
    w.WriteString("999.0.0");
    w.WriteString(Token);
    const auto mismatch = Roundtrip(rig.server, old, CommandType::Hello, w.Release());
    ASSERT_TRUE(mismatch.has_value());
    EXPECT_EQ(mismatch->type, ErrorType);
    EXPECT_NE(mismatch->Text().find("Protocol version mismatch"), std::string::npos) << mismatch->Text();

    EXPECT_EQ(rig.server.SessionHandoffs(), 0u) << "neither connection ever became a session";
    EXPECT_TRUE(PumpUntilLogged(rig.server, log, "wrong access token"));
    EXPECT_EQ(log.Count("Editor connected"), 0u);
}

// ==================== Stop ====================

TEST(PreAuthServerTest, StopWithEveryThreadBusyJoinsThemAllAndClosesTheirSockets)
{
    RigOptions options;
    options.busyGrace = 60s; // the waiting client is still waiting when Stop comes
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    // A session, two silent peers in their slots, and an authenticated client waiting for the session in a worker
    TestClient session(rig.server.GetPort());
    ASSERT_TRUE(session.IsConnected());
    const auto hello = rig.Hello(session);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    auto silentOne = rig.ConnectSilent(Remote(31));
    auto silentTwo = rig.ConnectSilent(Remote(32));
    TestClient waiting(rig.server.GetPort());
    ASSERT_TRUE(waiting.IsConnected());
    ASSERT_TRUE(waiting.SendRequest(CommandType::Hello, HelloPayload(Token)));
    Pump(rig.server, 300ms); // the worker has read the Hello and waits for the session
    EXPECT_EQ(rig.server.BusyRefusals(), 0u);

    const auto start = std::chrono::steady_clock::now();
    StopWithin(rig.server, 5s);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s) << "Stop returns within about a poll interval";
    EXPECT_FALSE(rig.server.IsRunning());
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 0u);

    // Every connection was closed by the server (sockets are closed after the joins)
    EXPECT_TRUE(session.IsClosedWithin(3000));
    EXPECT_TRUE(silentOne->IsClosedWithin(3000));
    EXPECT_TRUE(silentTwo->IsClosedWithin(3000));
    EXPECT_TRUE(waiting.IsClosedWithin(3000));
}

TEST(PreAuthServerTest, TheServerStartsAgainAfterStop)
{
    Rig rig;
    ASSERT_TRUE(rig.started);
    {
        TestClient client(rig.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = rig.Hello(client);
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    }
    StopWithin(rig.server, 5s);
    ASSERT_TRUE(rig.server.Start(0));

    TestClient again(rig.server.GetPort());
    ASSERT_TRUE(again.IsConnected());
    const auto hello = rig.Hello(again);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    EXPECT_EQ(rig.server.PreAuthSlotsInUse(), 0u);
}

// ==================== Settings ====================

TEST(PreAuthServerTest, TheSettingsAreValidatedAndLockedWhileRunning)
{
    EditorServer server;
    EXPECT_EQ(server.GetPreAuthSlots(), EditorServer::DefaultPreAuthSlots);
    EXPECT_EQ(server.GetPreAuthSlotsPerAddress(), EditorServer::DefaultPreAuthSlotsPerAddress);
    EXPECT_FALSE(server.HasHandshakeRateLimit());
    EXPECT_FALSE(server.SetPreAuthSlots(0));
    EXPECT_FALSE(server.SetPreAuthSlots(EditorServer::MaxPreAuthSlots + 1));
    EXPECT_EQ(server.GetPreAuthSlots(), EditorServer::DefaultPreAuthSlots);
    EXPECT_TRUE(server.SetPreAuthSlots(1));
    EXPECT_TRUE(server.SetPreAuthSlots(EditorServer::MaxPreAuthSlots));
    EXPECT_EQ(server.GetPreAuthSlots(), EditorServer::MaxPreAuthSlots);
    EXPECT_FALSE(server.SetPreAuthSlotsPerAddress(0));
    EXPECT_TRUE(server.SetPreAuthSlotsPerAddress(3));
    EXPECT_FALSE(server.SetBusyGrace(-1ms));

    ASSERT_TRUE(server.Start(0));
    EXPECT_FALSE(server.SetPreAuthSlots(2));
    EXPECT_FALSE(server.SetPreAuthSlotsPerAddress(2));
    EXPECT_FALSE(server.SetHandshakeRateLimit(TokenBucketOptions{}));
    EXPECT_FALSE(server.SetBusyGrace(1s));
    EXPECT_FALSE(server.SetPreHandoffHook({}));
    EXPECT_EQ(server.GetPreAuthSlots(), EditorServer::MaxPreAuthSlots);
    StopWithin(server, 10s);
}
