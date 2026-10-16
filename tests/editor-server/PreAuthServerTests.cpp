#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include <editor-server/EditorTls.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/TokenBucket.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Logger.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

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

    /// A user of a tokens file the tests log in as
    struct TestUser
    {
        std::string name;
        std::string token;
        TokenRole role;
    };

    const TestUser Alice{"alice", "alice-preauth-token-0001", TokenRole::Admin};
    const TestUser Zed{"zed", "zed-preauth-token-00000002", TokenRole::Admin};
    const TestUser Bob{"bob", "bob-preauth-token-000000003", TokenRole::Editor};
    const TestUser Erin{"erin", "erin-preauth-token-0000004", TokenRole::Editor};
    const TestUser Carol{"carol", "carol-preauth-token-000005", TokenRole::Viewer};
    const TestUser Dave{"dave", "dave-preauth-token-0000006", TokenRole::Viewer};

    std::vector<TestUser> AllUsers()
    {
        return {Alice, Zed, Bob, Erin, Carol, Dave};
    }

    /// A Hello as a 1.12+ client sends it: errorCodes, then the user whose token this is
    std::vector<uint8_t> HelloPayloadFor(const TestUser &user, const bool errorCodes)
    {
        BufferWriter w;
        w.WriteString("pre-auth test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(user.token);
        w.WriteBool(errorCodes);
        w.WriteString(user.name);
        return w.Release();
    }

    struct Coded
    {
        std::string message;
        std::string code;
    };

    Coded DecodeCoded(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ErrorCodedType) << frame.Text();
        BufferReader r(frame.payload);
        Coded coded;
        coded.message = r.ReadString();
        coded.code = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after ErrorCoded";
        return coded;
    }

    /// A tokens file of these users in a folder of its own; Ok() is false on a build without the TLS library's SHA-256
    class TokensFile
    {
    public:
        explicit TokensFile(const std::vector<TestUser> &users)
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _folder = fs::temp_directory_path() / ("n2-preauth-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_folder);
            _file = _folder / "tokens.json";
            std::string text = "[";
            for (size_t i = 0; i < users.size(); ++i)
            {
                const auto hash = HashToken(users[i].token);
                if (!hash.has_value())
                {
                    return;
                }
                text += (i == 0 ? "" : ",\n ") + std::string("{\"user\":\"") + users[i].name + "\",\"role\":\"" +
                        std::string(TokenRoleName(users[i].role)) + "\",\"hash\":\"" + FormatTokenHash(*hash) + "\"}";
            }
            text += "]\n";
            {
                std::ofstream out(_file, std::ios::binary | std::ios::trunc);
                out << text;
            }
#ifndef _WIN32
            fs::permissions(_file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
            _ok = true;
        }

        ~TokensFile()
        {
            std::error_code error;
            fs::remove_all(_folder, error);
        }

        TokensFile(const TokensFile &) = delete;
        TokensFile &operator=(const TokensFile &) = delete;

        [[nodiscard]] bool Ok() const { return _ok; }
        [[nodiscard]] const fs::path &Path() const { return _file; }

    private:
        fs::path _folder;
        fs::path _file;
        bool _ok = false;
    };

    /// A session that cannot end: its end hook blocks until released (or the server is stopping), as a session stuck in a
    /// write would. Declare it before the rig; set `server` once the rig exists.
    struct StuckSessions
    {
        std::atomic<bool> hold{false};
        std::atomic<int> entered{0};
        std::atomic<const EditorServer *> server{nullptr};

        EditorServer::SessionEndHook Hook()
        {
            return [this]
            {
                if (!hold.load())
                {
                    return;
                }
                ++entered;
                while (hold.load())
                {
                    const EditorServer *running = server.load();
                    if (running != nullptr && !running->IsRunning())
                    {
                        break; // Stop has begun: let the thread go
                    }
                    std::this_thread::sleep_for(5ms);
                }
            };
        }
    };

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
        std::optional<LogCoalescerOptions> logCoalescing;
        EditorServer::StopHook stopHook;
        /// A tokens file instead of the single token (users with roles)
        std::optional<fs::path> tokensFile;
        /// How long a client that takes the session over waits for the old one to end
        std::chrono::milliseconds takeoverWait = 10s;
        EditorServer::SessionStartHook sessionStartHook;
        EditorServer::SessionEndHook sessionEndHook;
    };

    /// A server whose peers the test chooses
    struct Rig
    {
        explicit Rig(const RigOptions &options = RigOptions{})
        {
            if (options.tokensFile.has_value())
            {
                EXPECT_TRUE(server.SetTokensFile(*options.tokensFile).has_value());
            }
            else if (options.token)
            {
                EXPECT_TRUE(server.SetAccessToken(Token));
            }
            EXPECT_TRUE(server.SetHelloTimeout(options.hello));
            EXPECT_TRUE(server.SetPreAuthSlots(options.slots));
            EXPECT_TRUE(server.SetBusyGrace(options.busyGrace));
            EXPECT_TRUE(server.SetTakeoverWait(options.takeoverWait));
            EXPECT_TRUE(server.SetSessionStartHook(options.sessionStartHook));
            EXPECT_TRUE(server.SetSessionEndHook(options.sessionEndHook));
            EXPECT_TRUE(server.SetAdmissionSummaryInterval(options.summaryInterval));
            EXPECT_TRUE(server.SetHandshakeRateLimit(options.bucket, options.bucketClock));
            EXPECT_TRUE(server.SetPeerAddressProvider([this](int) { return peer.Get(); }));
            if (options.readOnly)
            {
                EXPECT_TRUE(server.SetReadOnly(true));
            }
            if (options.logCoalescing.has_value())
            {
                EXPECT_TRUE(server.SetPreAuthLogCoalescing(*options.logCoalescing));
            }
            EXPECT_TRUE(server.SetStopHook(options.stopHook));
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

        /// A client that says Hello as this user (with the token and the user's name)
        std::optional<Frame> HelloAs(TestClient &client, const TestUser &user, const bool errorCodes = false)
        {
            return Roundtrip(server, client, CommandType::Hello, HelloPayloadFor(user, errorCodes));
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

// PR S-4 changed this test: a second client with the SAME token (one user, one role) used to be turned away with a plain
// Error; it is now the same user and role, so it takes the session over (the arbiter's rule). The Busy answers, for the
// users that do get them, are in the role matrix below.
TEST(PreAuthServerTest, ASecondClientWithTheSameTokenTakesTheSessionOver)
{
    RigOptions options;
    options.busyGrace = 300ms;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient first(rig.server.GetPort());
    ASSERT_TRUE(first.IsConnected());
    const auto firstHello = rig.Hello(first);
    ASSERT_TRUE(firstHello.has_value());
    ASSERT_EQ(firstHello->type, ServerInfoType) << firstHello->Text();

    TestClient second(rig.server.GetPort());
    ASSERT_TRUE(second.IsConnected());
    const auto secondHello = rig.Hello(second);
    ASSERT_TRUE(secondHello.has_value());
    EXPECT_EQ(secondHello->type, ServerInfoType) << secondHello->Text();
    EXPECT_TRUE(first.IsClosedWithin(5000)) << "the old session is closed for the new one";
    EXPECT_EQ(rig.server.SessionTakeovers(), 1u);
    EXPECT_EQ(rig.server.BusyRefusals(), 0u);

    const auto stillThere = Roundtrip(rig.server, second, CommandType::GetPlayState);
    ASSERT_TRUE(stillThere.has_value());
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

// PR S-4 changed this test (it was AHostWithoutATokenStillServesOneConnectionAtATime): the second connection used to wait
// in the backlog for as long as the first lasted; every client of a host without a token is an admin with no name, so
// the newest connection now takes over (design decision 8).
TEST(PreAuthServerTest, AHostWithoutATokenLetsTheNewestConnectionTakeOver)
{
    RigOptions options;
    options.token = false;
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient first(rig.server.GetPort());
    ASSERT_TRUE(first.IsConnected());
    const auto answered = Roundtrip(rig.server, first, CommandType::GetPlayState);
    ASSERT_TRUE(answered.has_value());

    // The second connects and sends a command: the first session ends (within a poll interval, with its cleanup) and the
    // second is served, without waiting for the first client to go
    TestClient second(rig.server.GetPort());
    ASSERT_TRUE(second.IsConnected());
    const auto response = Roundtrip(rig.server, second, CommandType::GetPlayState);
    ASSERT_TRUE(response.has_value());
    EXPECT_NE(response->type, ErrorType) << response->Text();
    EXPECT_TRUE(first.IsClosedWithin(5000));
    EXPECT_EQ(rig.server.SessionTakeovers(), 1u);
    EXPECT_EQ(rig.server.BusyRefusals(), 0u);

    // And a third takes over from the second
    TestClient third(rig.server.GetPort());
    ASSERT_TRUE(third.IsConnected());
    const auto last = Roundtrip(rig.server, third, CommandType::GetPlayState);
    ASSERT_TRUE(last.has_value());
    EXPECT_NE(last->type, ErrorType) << last->Text();
    EXPECT_TRUE(second.IsClosedWithin(5000));
    EXPECT_EQ(rig.server.SessionTakeovers(), 2u);
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

// ==================== What a failing pre-auth connection logs ====================

TEST(PreAuthServerTest, ABurstOfGarbageConnectionsWritesABoundedNumberOfLines)
{
    // Loopback (as behind a proxy): no backoff for a connection that sends garbage instead of a Hello, so nothing but
    // the coalescer stands between a loop of them and one line each in the main thread's queue
    LogCapture log;
    RigOptions options;
    options.logCoalescing = LogCoalescerOptions{60s, 3};
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    constexpr int Connections = 50;
    for (int i = 0; i < Connections; ++i)
    {
        ASSERT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; })) << i;
        TestClient garbage(rig.server.GetPort());
        ASSERT_TRUE(garbage.IsConnected()) << i;
        const auto response = Roundtrip(rig.server, garbage, CommandType::GetPlayState);
        ASSERT_TRUE(response.has_value()) << i;
        EXPECT_EQ(response->type, ErrorType) << i;
        EXPECT_EQ(response->Text(), EditorServer::HelloRequiredError) << i;
    }
    // Every connection was answered and counted, but only the first three wrote their line
    ASSERT_TRUE(WaitFor([&] { return rig.server.PreAuthLogLinesSuppressed() == static_cast<uint64_t>(Connections - 3); }))
        << rig.server.PreAuthLogLinesSuppressed();
    Pump(rig.server, 300ms);
    EXPECT_EQ(log.Count("it sent command 0x"), 3u);
    EXPECT_EQ(log.Count("hasn't sent a successful Hello"), 3u);
    // The start lines, "Waiting for editor connection..." and the three: a bound, not one line per connection
    EXPECT_LE(log.Lines().size(), 10u);
}

TEST(PreAuthServerTest, TheFailuresPastTheBurstAreReportedInOneSummaryLineOnceTheWindowHasPassed)
{
    LogCapture log;
    RigOptions options;
    options.logCoalescing = LogCoalescerOptions{1000ms, 1};
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    for (int i = 0; i < 5; ++i)
    {
        ASSERT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0; })) << i;
        TestClient garbage(rig.server.GetPort());
        ASSERT_TRUE(garbage.IsConnected()) << i;
        const auto response = Roundtrip(rig.server, garbage, CommandType::GetPlayState);
        ASSERT_TRUE(response.has_value()) << i;
    }
    ASSERT_TRUE(WaitFor([&] { return rig.server.PreAuthLogLinesSuppressed() > 0; }));
    // The first was written at once; the acceptor reports the rest within about a second of the window's end
    EXPECT_TRUE(PumpUntilLogged(rig.server, log, "it sent command 0x"));
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "pre-auth failure(s) of kind 'command before Hello'"));
    EXPECT_TRUE(log.Has("were not logged one by one (at most 1 line(s) per kind every 1 s are)"));
}

// ==================== Stop ====================

TEST(PreAuthServerTest, StopWithEveryThreadBusyJoinsThemWithinASecondAndEachGaveItsSlotBack)
{
    // What Stop saw after its joins, before it closed what was left (the first Stop only: the rig stops again at the end)
    std::mutex reportsMutex;
    std::vector<EditorServer::StopReport> reports;
    StuckSessions stuck; // the session cannot end by itself, so the client that takes it over is still waiting when Stop comes
    RigOptions options;
    options.busyGrace = 60s;
    options.takeoverWait = 60s;
    options.sessionEndHook = stuck.Hook();
    options.stopHook = [&](const EditorServer::StopReport &report)
    {
        const std::lock_guard lock(reportsMutex);
        reports.push_back(report);
    };
    Rig rig(options);
    ASSERT_TRUE(rig.started);
    stuck.server = &rig.server;

    // A session in its idle wait, two silent peers in their Hello wait, and an authenticated client waiting for the
    // session in a worker: every kind of thread is inside a wait when Stop comes
    TestClient session(rig.server.GetPort());
    ASSERT_TRUE(session.IsConnected());
    const auto hello = rig.Hello(session);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    stuck.hold = true;
    auto silentOne = rig.ConnectSilent(Remote(31));
    auto silentTwo = rig.ConnectSilent(Remote(32));
    TestClient waiting(rig.server.GetPort());
    ASSERT_TRUE(waiting.IsConnected());
    ASSERT_TRUE(waiting.SendRequest(CommandType::Hello, HelloPayload(Token)));
    Pump(rig.server, 300ms); // the worker has read the Hello, asked the session to end, and waits for it
    EXPECT_EQ(rig.server.BusyRefusals(), 0u);
    ASSERT_EQ(rig.server.SessionTakeovers(), 1u);
    ASSERT_EQ(rig.server.PreAuthSlotsInUse(), 3u) << "two silent peers and the waiting client hold a slot each";

    // Every wait is sliced at 100 ms and looks at the stop flag: Stop returns within a small multiple of that, not
    // after the 1 s busy grace, the Hello budget or the 60 s waits given here
    const auto start = std::chrono::steady_clock::now();
    StopWithin(rig.server, 5s);
    const auto took = std::chrono::steady_clock::now() - start;
    EXPECT_LT(took, 1s) << "Stop took " << std::chrono::duration_cast<std::chrono::milliseconds>(took).count() << " ms";
    EXPECT_FALSE(rig.server.IsRunning());

    {
        const std::lock_guard lock(reportsMutex);
        ASSERT_FALSE(reports.empty()) << "the stop hook was called";
        const EditorServer::StopReport &report = reports.front();
        EXPECT_LT(report.joinTime, std::chrono::steady_clock::duration(1s));
        // Each worker gave its slot back as it left (none was left queued for Stop to close): a slot a thread failed to
        // release would still be counted here
        EXPECT_EQ(report.leftoverConnections, 0u);
        EXPECT_EQ(report.slotsHeldAfterJoins, 0u);
        EXPECT_EQ(report.slotsHeldAfterCleanup, 0u);
    }

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

// ==================== Preemption, takeover and Busy (PR S-4) ====================

// The arbiter's rule on real sockets (SessionArbiterTests has it as a pure function): a client of the same user and role
// takes the session over, a strictly higher role preempts it, anybody else is turned away. The users need a tokens
// file, which needs the TLS library's SHA-256 (these tests skip themselves without it).

namespace
{
    constexpr const char *NoTokensFileSupport = "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";

    RigOptions RoleOptions(const TokensFile &tokens)
    {
        RigOptions options;
        options.tokensFile = tokens.Path();
        options.busyGrace = 300ms; // the wait for a session that is not ending
        options.takeoverWait = 10s;
        return options;
    }

    /// `active` has the session; `candidate` says Hello and must take it from it, by role
    void ExpectPreempts(const TestUser &active, const TestUser &candidate)
    {
        TokensFile tokens(AllUsers());
        if (!tokens.Ok())
        {
            GTEST_SKIP() << NoTokensFileSupport;
        }
        Rig rig(RoleOptions(tokens));
        ASSERT_TRUE(rig.started);

        TestClient old(rig.server.GetPort());
        ASSERT_TRUE(old.IsConnected());
        const auto oldHello = rig.HelloAs(old, active);
        ASSERT_TRUE(oldHello.has_value());
        ASSERT_EQ(oldHello->type, ServerInfoType) << oldHello->Text();

        TestClient next(rig.server.GetPort());
        ASSERT_TRUE(next.IsConnected());
        const auto nextHello = rig.HelloAs(next, candidate);
        ASSERT_TRUE(nextHello.has_value());
        ASSERT_EQ(nextHello->type, ServerInfoType) << candidate.name << " over " << active.name << ": " << nextHello->Text();
        EXPECT_TRUE(old.IsClosedWithin(5000)) << "the lower role's session is closed";

        EXPECT_EQ(rig.server.SessionPreemptions(), 1u);
        EXPECT_EQ(rig.server.SessionTakeovers(), 0u);
        EXPECT_EQ(rig.server.BusyRefusals(), 0u);
        const auto works = Roundtrip(rig.server, next, CommandType::GetPlayState);
        ASSERT_TRUE(works.has_value());
        EXPECT_NE(works->type, ErrorType) << works->Text();
    }

    /// `active` has the session; `candidate` is turned away, with a plain Error, and then (asking for error codes) with
    /// ErrorCoded `busy`. The session is untouched.
    void ExpectBusy(const TestUser &active, const TestUser &candidate)
    {
        TokensFile tokens(AllUsers());
        if (!tokens.Ok())
        {
            GTEST_SKIP() << NoTokensFileSupport;
        }
        Rig rig(RoleOptions(tokens));
        ASSERT_TRUE(rig.started);

        TestClient holder(rig.server.GetPort());
        ASSERT_TRUE(holder.IsConnected());
        const auto holderHello = rig.HelloAs(holder, active);
        ASSERT_TRUE(holderHello.has_value());
        ASSERT_EQ(holderHello->type, ServerInfoType) << holderHello->Text();
        const std::string expected = EditorServer::BusyMessage(active.role);

        {
            // A client that did not ask for error codes: the plain Error it always got
            TestClient plain(rig.server.GetPort());
            ASSERT_TRUE(plain.IsConnected());
            const auto answer = rig.HelloAs(plain, candidate, false);
            ASSERT_TRUE(answer.has_value());
            EXPECT_EQ(answer->type, ErrorType) << candidate.name << " against " << active.name;
            EXPECT_EQ(answer->Text(), expected);
            EXPECT_TRUE(plain.IsClosedWithin(5000));
        }
        {
            // One that did: ErrorCoded with the code `busy` and the same text
            TestClient coded(rig.server.GetPort());
            ASSERT_TRUE(coded.IsConnected());
            const auto answer = rig.HelloAs(coded, candidate, true);
            ASSERT_TRUE(answer.has_value());
            ASSERT_EQ(answer->type, ErrorCodedType) << answer->Text();
            const Coded decoded = DecodeCoded(*answer);
            EXPECT_EQ(decoded.code, std::string(ErrorCodeBusy));
            EXPECT_EQ(decoded.message, expected);
            EXPECT_TRUE(coded.IsClosedWithin(5000));
        }

        EXPECT_EQ(rig.server.BusyRefusals(), 2u);
        EXPECT_EQ(rig.server.SessionPreemptions(), 0u);
        EXPECT_EQ(rig.server.SessionTakeovers(), 0u);
        // The text names the role of the session it was turned away from, never its user
        EXPECT_NE(expected.find(std::string(TokenRoleName(active.role))), std::string::npos);
        EXPECT_EQ(expected.find(active.name), std::string::npos);
        const auto stillThere = Roundtrip(rig.server, holder, CommandType::GetPlayState);
        ASSERT_TRUE(stillThere.has_value()) << "the session was not touched";
        EXPECT_NE(stillThere->type, ErrorType) << stillThere->Text();
    }

    /// The same user, again: the newest connection has the session
    void ExpectTakeover(const TestUser &user)
    {
        TokensFile tokens(AllUsers());
        if (!tokens.Ok())
        {
            GTEST_SKIP() << NoTokensFileSupport;
        }
        Rig rig(RoleOptions(tokens));
        ASSERT_TRUE(rig.started);

        TestClient old(rig.server.GetPort());
        ASSERT_TRUE(old.IsConnected());
        const auto oldHello = rig.HelloAs(old, user);
        ASSERT_TRUE(oldHello.has_value());
        ASSERT_EQ(oldHello->type, ServerInfoType) << oldHello->Text();

        TestClient next(rig.server.GetPort());
        ASSERT_TRUE(next.IsConnected());
        const auto nextHello = rig.HelloAs(next, user);
        ASSERT_TRUE(nextHello.has_value());
        ASSERT_EQ(nextHello->type, ServerInfoType) << nextHello->Text();
        EXPECT_TRUE(old.IsClosedWithin(5000)) << "a client that lost its connection reconnects at once";

        EXPECT_EQ(rig.server.SessionTakeovers(), 1u);
        EXPECT_EQ(rig.server.SessionPreemptions(), 0u);
        EXPECT_EQ(rig.server.BusyRefusals(), 0u);
        const auto works = Roundtrip(rig.server, next, CommandType::GetPlayState);
        ASSERT_TRUE(works.has_value());
        EXPECT_NE(works->type, ErrorType) << works->Text();
    }
}

TEST(SessionPolicyTest, AnEditorPreemptsAViewer)
{
    ExpectPreempts(Carol, Bob);
}

TEST(SessionPolicyTest, AnAdminPreemptsAViewer)
{
    ExpectPreempts(Carol, Alice);
}

TEST(SessionPolicyTest, AnAdminPreemptsAnEditor)
{
    ExpectPreempts(Bob, Alice);
}

TEST(SessionPolicyTest, AViewerIsTurnedAwayFromAnEditorsSession)
{
    ExpectBusy(Bob, Carol);
}

TEST(SessionPolicyTest, AViewerIsTurnedAwayFromAnAdminsSession)
{
    ExpectBusy(Alice, Carol);
}

TEST(SessionPolicyTest, AnEditorIsTurnedAwayFromAnAdminsSession)
{
    ExpectBusy(Alice, Bob);
}

TEST(SessionPolicyTest, AnotherUserOfTheSameRoleIsTurnedAway)
{
    ExpectBusy(Bob, Erin);
}

TEST(SessionPolicyTest, AnotherAdminIsTurnedAwayFromAnAdminsSession)
{
    ExpectBusy(Alice, Zed);
}

TEST(SessionPolicyTest, AnotherViewerIsTurnedAwayFromAViewersSession)
{
    ExpectBusy(Carol, Dave);
}

TEST(SessionPolicyTest, TheSameUserTakesTheSessionOver)
{
    ExpectTakeover(Bob);
}

TEST(SessionPolicyTest, TheSameAdminTakesTheSessionOver)
{
    ExpectTakeover(Alice);
}

TEST(SessionPolicyTest, TheSameViewerTakesTheSessionOver)
{
    ExpectTakeover(Carol);
}

TEST(SessionPolicyTest, ALaterHelloOnTheSessionChangesWhatTheArbiterJudgesAgainst)
{
    TokensFile tokens(AllUsers());
    if (!tokens.Ok())
    {
        GTEST_SKIP() << NoTokensFileSupport;
    }
    Rig rig(RoleOptions(tokens));
    ASSERT_TRUE(rig.started);

    // carol starts as a viewer and says Hello again on the same connection with bob's token: the session is now bob's, an
    // editor's. Judged against the Hello it started with (a viewer), erin (an editor) would preempt it; judged against
    // what the session is now, she is the same role as another user and is turned away.
    TestClient session(rig.server.GetPort());
    ASSERT_TRUE(session.IsConnected());
    const auto first = rig.HelloAs(session, Carol);
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(first->type, ServerInfoType) << first->Text();
    const auto second = rig.HelloAs(session, Bob);
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(second->type, ServerInfoType) << second->Text();

    TestClient candidate(rig.server.GetPort());
    ASSERT_TRUE(candidate.IsConnected());
    const auto answer = rig.HelloAs(candidate, Erin);
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->type, ErrorType) << "turned away";
    EXPECT_EQ(answer->Text(), EditorServer::BusyMessage(TokenRole::Editor));
    EXPECT_EQ(rig.server.SessionPreemptions(), 0u);
    const auto stillThere = Roundtrip(rig.server, session, CommandType::GetPlayState);
    ASSERT_TRUE(stillThere.has_value());
    EXPECT_NE(stillThere->type, ErrorType) << stillThere->Text();
}

TEST(SessionPolicyTest, PreemptionEndsTheOldSessionAsADisconnectDoesBeforeTheNewHelloRuns)
{
    TokensFile tokens(AllUsers());
    if (!tokens.Ok())
    {
        GTEST_SKIP() << NoTokensFileSupport;
    }
    LogCapture log;
    Rig rig(RoleOptions(tokens));
    ASSERT_TRUE(rig.started);

    // bob opens an edit group and leaves it open
    TestClient editor(rig.server.GetPort());
    ASSERT_TRUE(editor.IsConnected());
    const auto editorHello = rig.HelloAs(editor, Bob);
    ASSERT_TRUE(editorHello.has_value());
    ASSERT_EQ(editorHello->type, ServerInfoType) << editorHello->Text();
    BufferWriter label;
    label.WriteString("Left open");
    const auto begun = Roundtrip(rig.server, editor, CommandType::BeginEditGroup, label.Release());
    ASSERT_TRUE(begun.has_value());
    ASSERT_EQ(begun->type, static_cast<uint8_t>(ResponseType::Ok)) << begun->Text();
    ASSERT_TRUE(rig.server.GetHistory().InGroup());

    // alice preempts him
    TestClient admin(rig.server.GetPort());
    ASSERT_TRUE(admin.IsConnected());
    const auto adminHello = rig.HelloAs(admin, Alice);
    ASSERT_TRUE(adminHello.has_value());
    ASSERT_EQ(adminHello->type, ServerInfoType) << adminHello->Text();
    EXPECT_TRUE(editor.IsClosedWithin(5000));
    EXPECT_FALSE(rig.server.GetHistory().InGroup()) << "the group he left open is closed";

    // The old session's end (and the cleanup it queues) is logged, and runs, before the new Hello does: both go through
    // the one FIFO command queue
    ASSERT_TRUE(PumpUntilLogged(rig.server, log, "user=alice role=admin"));
    const std::vector<std::string> lines = log.Lines();
    size_t disconnected = lines.size();
    size_t preempted = lines.size();
    size_t helloLine = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (disconnected == lines.size() && lines[i].find("Editor disconnected") != std::string::npos)
            disconnected = i;
        if (preempted == lines.size() && lines[i].find("Session preemption: the session of user=bob role=editor") != std::string::npos)
            preempted = i;
        if (helloLine == lines.size() && lines[i].find("said Hello") != std::string::npos &&
            lines[i].find("user=alice role=admin") != std::string::npos)
            helloLine = i;
    }
    ASSERT_NE(preempted, lines.size()) << "the preemption is logged";
    ASSERT_NE(disconnected, lines.size());
    ASSERT_NE(helloLine, lines.size());
    EXPECT_LT(preempted, disconnected);
    EXPECT_LT(disconnected, helloLine) << "the old session ended before the new Hello ran";
}

TEST(SessionPolicyTest, TheCandidateGetsBusyIfTheOldSessionDoesNotEndInTime)
{
    TokensFile tokens(AllUsers());
    if (!tokens.Ok())
    {
        GTEST_SKIP() << NoTokensFileSupport;
    }
    StuckSessions stuck;
    RigOptions options = RoleOptions(tokens);
    options.takeoverWait = 400ms;
    options.sessionEndHook = stuck.Hook();
    Rig rig(options);
    ASSERT_TRUE(rig.started);
    stuck.server = &rig.server;

    TestClient editor(rig.server.GetPort());
    ASSERT_TRUE(editor.IsConnected());
    const auto editorHello = rig.HelloAs(editor, Bob);
    ASSERT_TRUE(editorHello.has_value());
    ASSERT_EQ(editorHello->type, ServerInfoType) << editorHello->Text();

    // From here the session cannot end: it is asked to (the admin preempts), and does not
    stuck.hold = true;
    TestClient admin(rig.server.GetPort());
    ASSERT_TRUE(admin.IsConnected());
    const auto start = std::chrono::steady_clock::now();
    const auto turnedAway = rig.HelloAs(admin, Alice, true);
    const auto waited = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(turnedAway.has_value());
    ASSERT_EQ(turnedAway->type, ErrorCodedType) << turnedAway->Text();
    const Coded coded = DecodeCoded(*turnedAway);
    EXPECT_EQ(coded.code, std::string(ErrorCodeBusy));
    EXPECT_EQ(coded.message, EditorServer::BusyMessage(TokenRole::Editor));
    EXPECT_GE(waited, 350ms) << "it waited the takeover wait for the old session";
    EXPECT_TRUE(admin.IsClosedWithin(5000));
    EXPECT_EQ(stuck.entered.load(), 1) << "the old session was asked to end, and is stuck at its end";
    EXPECT_EQ(rig.server.SessionPreemptions(), 1u);
    EXPECT_EQ(rig.server.BusyRefusals(), 1u);
    EXPECT_FALSE(editor.IsClosedWithin(50)) << "its connection is still open: its thread has not finished";

    // It does end once it is let go, and then the admin gets in
    stuck.hold = false;
    EXPECT_TRUE(editor.IsClosedWithin(5000));
    TestClient again(rig.server.GetPort());
    ASSERT_TRUE(again.IsConnected());
    const auto retried = rig.HelloAs(again, Alice);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(retried->type, ServerInfoType) << retried->Text();
}

TEST(SessionPolicyTest, ASecondCandidateDuringATakeoverIsJudgedAgainstTheOneThatClaimedTheHost)
{
    TokensFile tokens(AllUsers());
    if (!tokens.Ok())
    {
        GTEST_SKIP() << NoTokensFileSupport;
    }
    StuckSessions stuck;
    RigOptions options = RoleOptions(tokens);
    options.sessionEndHook = stuck.Hook();
    Rig rig(options);
    ASSERT_TRUE(rig.started);
    stuck.server = &rig.server;

    TestClient editor(rig.server.GetPort());
    ASSERT_TRUE(editor.IsConnected());
    const auto editorHello = rig.HelloAs(editor, Bob);
    ASSERT_TRUE(editorHello.has_value());
    ASSERT_EQ(editorHello->type, ServerInfoType) << editorHello->Text();
    stuck.hold = true; // the old session stays until the end of the test, so the claims below are all waiting

    // alice preempts bob and waits for him to end
    TestClient first(rig.server.GetPort());
    ASSERT_TRUE(first.IsConnected());
    ASSERT_TRUE(first.SendRequest(CommandType::Hello, HelloPayloadFor(Alice, false)));
    ASSERT_TRUE(WaitFor([&] { return rig.server.SessionPreemptions() == 1u; }));

    // alice again (the same user and role as the claim): she takes the host from the first one while it waits
    TestClient second(rig.server.GetPort());
    ASSERT_TRUE(second.IsConnected());
    ASSERT_TRUE(second.SendRequest(CommandType::Hello, HelloPayloadFor(Alice, false)));
    ASSERT_TRUE(WaitFor([&] { return rig.server.SessionTakeovers() == 1u; }));
    const auto overtaken = first.ReadResponse();
    ASSERT_TRUE(overtaken.has_value());
    EXPECT_EQ(Decode(*overtaken).type, ErrorType);
    EXPECT_EQ(Decode(*overtaken).Text(), EditorServer::BusyError) << "turned away by a newer claim: no session to name";
    EXPECT_TRUE(first.IsClosedWithin(5000));

    // carol (a viewer) does not outrank the claim: she is turned away, and the answer names the claim's role (admin)
    TestClient viewer(rig.server.GetPort());
    ASSERT_TRUE(viewer.IsConnected());
    const auto viewerHello = rig.HelloAs(viewer, Carol);
    ASSERT_TRUE(viewerHello.has_value());
    EXPECT_EQ(viewerHello->type, ErrorType);
    EXPECT_EQ(viewerHello->Text(), EditorServer::BusyMessage(TokenRole::Admin));

    // Once the old session is let go, the one claim left is served, and nobody else
    stuck.hold = false;
    const auto limit = std::chrono::steady_clock::now() + 10s;
    while (second.BytesAvailable() < 5 && std::chrono::steady_clock::now() < limit)
    {
        rig.server.ProcessCommands(10ms);
    }
    const auto served = second.ReadResponse();
    ASSERT_TRUE(served.has_value());
    EXPECT_EQ(Decode(*served).type, ServerInfoType) << Decode(*served).Text();
    EXPECT_TRUE(editor.IsClosedWithin(5000));
    EXPECT_EQ(rig.server.SessionHandoffs(), 2u) << "bob's, and the one claim that stood";
}

TEST(SessionPolicyTest, ACandidateThatHungUpWhileItWaitedGivesItsSlotBack)
{
    StuckSessions stuck;
    RigOptions options;
    options.token = false; // the newest connection takes over, and waits for the old session (which is stuck)
    options.sessionEndHook = stuck.Hook();
    LogCapture log;
    Rig rig(options);
    ASSERT_TRUE(rig.started);
    stuck.server = &rig.server;

    auto first = std::make_unique<TestClient>(rig.server.GetPort());
    ASSERT_TRUE(first->IsConnected());
    ASSERT_TRUE(Roundtrip(rig.server, *first, CommandType::GetPlayState).has_value());
    stuck.hold = true;

    {
        TestClient waiter(rig.server.GetPort());
        ASSERT_TRUE(waiter.IsConnected());
        ASSERT_TRUE(WaitFor([&] { return rig.server.SessionTakeovers() == 1u; }));
        ASSERT_EQ(rig.server.PreAuthSlotsInUse(), 1u) << "it holds a slot while it waits";
    } // it hangs up

    EXPECT_TRUE(WaitFor([&] { return rig.server.PreAuthSlotsInUse() == 0u; }, 5s))
        << "the slot goes back at once, not when the old session finally ends";
    EXPECT_TRUE(PumpUntilLogged(rig.server, log, "A client waiting for the session hung up"));

    // The old session ends once it is let go, and the host serves the next client: no claim was left behind
    stuck.hold = false;
    EXPECT_TRUE(first->IsClosedWithin(5000));
    TestClient next(rig.server.GetPort());
    ASSERT_TRUE(next.IsConnected());
    const auto response = Roundtrip(rig.server, next, CommandType::GetPlayState);
    ASSERT_TRUE(response.has_value());
    EXPECT_NE(response->type, ErrorType) << response->Text();
}

// ==================== TCP keepalive on the session socket ====================

namespace
{
    struct KeepAliveProbe
    {
        std::mutex mutex;
        bool seen = false;
        int enabled = -1;
        int idle = -1;
        int interval = -1;
        int count = -1;
    };

    int ReadSocketOption(const int socketHandle, const int level, const int option)
    {
        int value = -1;
#ifdef _WIN32
        int size = sizeof(value);
        if (getsockopt(static_cast<SOCKET>(socketHandle), level, option, reinterpret_cast<char *>(&value), &size) != 0)
            return -1;
#else
        socklen_t size = sizeof(value);
        if (getsockopt(socketHandle, level, option, &value, &size) != 0)
            return -1;
#endif
        return value;
    }
}

TEST(PreAuthServerTest, TheSessionSocketHasTcpKeepAlive)
{
    KeepAliveProbe probe;
    RigOptions options;
    options.sessionStartHook = [&probe](const int socketHandle)
    {
        const std::lock_guard lock(probe.mutex);
        probe.enabled = ReadSocketOption(socketHandle, SOL_SOCKET, SO_KEEPALIVE);
#if defined(__linux__)
        probe.idle = ReadSocketOption(socketHandle, IPPROTO_TCP, TCP_KEEPIDLE);
        probe.interval = ReadSocketOption(socketHandle, IPPROTO_TCP, TCP_KEEPINTVL);
        probe.count = ReadSocketOption(socketHandle, IPPROTO_TCP, TCP_KEEPCNT);
#elif defined(__APPLE__)
        probe.idle = ReadSocketOption(socketHandle, IPPROTO_TCP, TCP_KEEPALIVE);
        probe.interval = ReadSocketOption(socketHandle, IPPROTO_TCP, TCP_KEEPINTVL);
        probe.count = ReadSocketOption(socketHandle, IPPROTO_TCP, TCP_KEEPCNT);
#endif
        probe.seen = true;
    };
    Rig rig(options);
    ASSERT_TRUE(rig.started);

    TestClient client(rig.server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello = rig.Hello(client);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

    ASSERT_TRUE(WaitFor(
        [&]
        {
            const std::lock_guard lock(probe.mutex);
            return probe.seen;
        }));
    const std::lock_guard lock(probe.mutex);
    EXPECT_NE(probe.enabled, 0) << "SO_KEEPALIVE is on";
    EXPECT_NE(probe.enabled, -1) << "and could be read";
#if defined(__linux__) || defined(__APPLE__)
    EXPECT_EQ(probe.idle, static_cast<int>(Connection::KeepAliveIdle.count()));
    EXPECT_EQ(probe.interval, static_cast<int>(Connection::KeepAliveInterval.count()));
    EXPECT_EQ(probe.count, Connection::KeepAliveCount);
#endif
    // Windows: SIO_KEEPALIVE_VALS sets the times, and they can't be read back; the option above is what is checked
}
