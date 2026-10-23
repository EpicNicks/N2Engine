#include <gtest/gtest.h>

#include <array>
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
#include <editor-server/RateLimiter.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Logger.hpp>

// Last, as in EditorServer.cpp: <windows.h> macros (near, far, ...) must not reach the engine headers
#include "TestSockets.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
namespace fs = std::filesystem;

// Failed-Hello rate limiting on a real server, over loopback sockets. The peer address comes from an injected provider
// (so a connection made on loopback can be any address, IPv6 included) and the time from an injected clock, so nothing
// here sleeps: a backoff of a second or a quarter of an hour is a number the test moves the clock by.
// RateLimiterTests.cpp covers the tables themselves.

namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr const char *Token = "rate-limit-token";

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

    std::vector<uint8_t> HelloPayload(const std::string &token, const std::string &version = std::string{ProtocolVersion},
                                      const std::optional<std::string> &user = std::nullopt)
    {
        BufferWriter w;
        w.WriteString("rate limit test");
        w.WriteString(version);
        w.WriteString(token);
        if (user.has_value())
        {
            w.WriteBool(true); // errorCodes: a user can only follow it
            w.WriteString(*user);
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

        /// True once the server has closed (or reset) the connection without sending anything more; a receive timeout
        /// doesn't count, and neither does a byte (so a server that answered a banned peer would fail this)
        bool IsClosedByServer()
        {
            std::uint8_t byte = 0;
            const long long result = N2TestSockets::Receive(_socket, &byte, 1);
            return result == 0 || (result < 0 && !N2TestSockets::LastErrorWasTimeout());
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (client.BytesAvailable() < 5 && std::chrono::steady_clock::now() < deadline)
        {
            server.ProcessCommands(std::chrono::milliseconds(10));
        }
        const auto response = client.ReadResponse();
        if (!response.has_value())
            return std::nullopt;
        return Decode(*response);
    }

    /// Plays the host's main loop until the log has this text (the network thread posts its lines to it)
    bool PumpUntilLogged(EditorServer &server, const LogCapture &log, const std::string_view text)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!log.Has(text) && std::chrono::steady_clock::now() < deadline)
        {
            server.ProcessCommands(std::chrono::milliseconds(10));
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

    /// A server with the single access token, whose peers and clock the test chooses. The Hello timeout is long, so a
    /// connection that is held open rather than closed would show as a client waiting out its receive timeout.
    struct RateLimitedServer
    {
        explicit RateLimitedServer(const RateLimitPolicy &policy = {}, const bool useToken = true,
                                   const bool rateLimiting = true)
        {
            EXPECT_TRUE(server.SetRateLimiting(rateLimiting));
            if (useToken)
            {
                EXPECT_TRUE(server.SetAccessToken(Token));
            }
            EXPECT_TRUE(server.SetHelloTimeout(std::chrono::seconds(30)));
            EXPECT_TRUE(server.SetRateLimitPolicy(policy));
            EXPECT_TRUE(server.SetRateLimitClock([this] { return RateLimiter::TimePoint(std::chrono::milliseconds(nowMs)); }));
            EXPECT_TRUE(server.SetPeerAddressProvider([this](int) { return peer.Get(); }));
            started = server.Start(0);
        }

        ~RateLimitedServer() { StopWithin(server, std::chrono::seconds(10)); }

        void SetPeer(const PeerAddress &address) { peer.Set(address); }

        void Advance(const std::chrono::milliseconds by) { nowMs += by.count(); }

        /// A connection that sends a Hello with this token (and user) and reads the answer; the answer's frame
        std::optional<Frame> Hello(const std::string &token, const std::optional<std::string> &user = std::nullopt,
                                   const std::string &version = std::string{ProtocolVersion})
        {
            TestClient client(server.GetPort());
            if (!client.IsConnected())
                return std::nullopt;
            return Roundtrip(server, client, CommandType::Hello, HelloPayload(token, version, user));
        }

        /// Fails `count` Hellos (a wrong token), each on a connection of its own that the server then closes
        void FailHellos(const int count, const std::optional<std::string> &user = std::nullopt)
        {
            for (int i = 0; i < count; ++i)
            {
                TestClient client(server.GetPort());
                ASSERT_TRUE(client.IsConnected());
                const auto response = Roundtrip(server, client, CommandType::Hello, HelloPayload("wrong-token", std::string{ProtocolVersion}, user));
                ASSERT_TRUE(response.has_value()) << i;
                EXPECT_EQ(response->type, ErrorType) << i;
                EXPECT_EQ(response->Text(), "Invalid access token") << i;
                EXPECT_TRUE(client.IsClosedByServer()) << i;
            }
        }

        /// Whether a connection is closed at accept: it sends nothing and the server closes it, long before the Hello
        /// timeout, without a byte
        bool ClosedAtAccept()
        {
            TestClient client(server.GetPort());
            if (!client.IsConnected())
                return false;
            return client.IsClosedByServer();
        }

        static inline const PeerAddress Attacker = PeerAddress::IPv4(203, 0, 113, 7);
        static inline const PeerAddress Friend = PeerAddress::IPv4(198, 51, 100, 20);
        static inline const PeerAddress Loopback = PeerAddress::IPv4(127, 0, 0, 1);

        // Before the server: it is stopped first, and its thread reads both
        SharedPeer peer{Attacker};
        std::atomic<int64_t> nowMs{1000};
        EditorServer server;
        bool started = false;
    };
}

// ==================== Banned peers are closed before any work ====================

TEST(RateLimitServerTest, ABannedPeerIsClosedAtAcceptWithoutASingleByteAndAGoodPeerIsUnaffected)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);

    host.FailHellos(3);
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u);

    // The fourth connection from the same address: closed by the server with nothing sent and nothing awaited (the Hello
    // timeout is 30 s, the client's receive timeout 10), even though it never says Hello
    EXPECT_TRUE(host.ClosedAtAccept());
    EXPECT_EQ(host.server.RateLimitedConnections(), 1u);
    {
        // Even a Hello with the right token is not read: the connection is gone first
        TestClient client(host.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        (void)client.SendRequest(CommandType::Hello, HelloPayload(Token));
        EXPECT_TRUE(client.IsClosedByServer());
        EXPECT_EQ(client.BytesAvailable(), 0u);
    }
    EXPECT_EQ(host.server.RateLimitedConnections(), 2u);

    // A good user from another address, meanwhile, is served as usual
    host.SetPeer(RateLimitedServer::Friend);
    const auto hello = host.Hello(Token);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    EXPECT_EQ(host.server.RateLimitedConnections(), 2u);

    // The attacker's backoff ends with the clock, not with anyone sleeping
    host.SetPeer(RateLimitedServer::Attacker);
    host.Advance(std::chrono::milliseconds(999));
    EXPECT_TRUE(host.ClosedAtAccept());
    host.Advance(std::chrono::milliseconds(1));
    const auto again = host.Hello(Token);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->type, ServerInfoType) << again->Text();
}

TEST(RateLimitServerTest, TheBackoffDoublesAndASuccessClearsIt)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);

    host.FailHellos(3); // 1 s
    host.Advance(std::chrono::milliseconds(1000));
    host.FailHellos(1); // the next try fails: 2 s
    host.Advance(std::chrono::milliseconds(1999));
    EXPECT_TRUE(host.ClosedAtAccept());
    host.Advance(std::chrono::milliseconds(1));

    // A good Hello from the address forgets its failures: three more are needed to ban it again
    const auto hello = host.Hello(Token);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    host.FailHellos(2);
    const auto stillGood = host.Hello(Token);
    ASSERT_TRUE(stillGood.has_value());
    EXPECT_EQ(stillGood->type, ServerInfoType) << stillGood->Text();
}

TEST(RateLimitServerTest, OnlyAWrongTokenCountsNotAVersionMismatch)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);

    // Refused, and the connection closed, but for the protocol version: the token was right
    for (int i = 0; i < 6; ++i)
    {
        const auto response = host.Hello(Token, std::nullopt, "999.0.0");
        ASSERT_TRUE(response.has_value()) << i;
        EXPECT_EQ(response->type, ErrorType) << i;
    }
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u);
    const auto hello = host.Hello(Token);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
}

TEST(RateLimitServerTest, AMalformedHelloCountsAsAFailureOfItsAddress)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);

    // A Hello header with no payload does not parse: refused, and the connection closed, but not for the version
    for (int i = 0; i < 3; ++i)
    {
        TestClient client(host.server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto response = Roundtrip(host.server, client, CommandType::Hello);
        ASSERT_TRUE(response.has_value()) << i;
        EXPECT_EQ(response->type, ErrorType) << i;
        EXPECT_TRUE(client.IsClosedByServer()) << i;
    }
    EXPECT_TRUE(host.ClosedAtAccept());
}

TEST(RateLimitServerTest, APeerWithFailuresOnRecordGetsTheShortBudget)
{
    RateLimitPolicy policy;
    policy.priorOffenderBudget = std::chrono::milliseconds(50);
    RateLimitedServer host(policy);
    ASSERT_TRUE(host.started);

    host.FailHellos(1); // on record, not yet backed off
    // It connects and says nothing: closed after the short budget (50 ms), not the Hello timeout (30 s) the client
    // would otherwise wait out. This waits on the server's close, with no sleep.
    EXPECT_TRUE(host.ClosedAtAccept());
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u) << "it was served, then timed out; it wasn't refused at accept";
}

// ==================== Loopback ====================

// PR S-4: the shared loopback backoff (ten failed Hellos in a minute refused every loopback connection, a client with
// the right token included) is replaced by the caps on pre-auth slots and handshake rate (PreAuthServerTests)
TEST(RateLimitServerTest, LoopbackIsExemptFromTheAddressBanAndNoFloodOfWrongTokensLocksItOut)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);
    host.SetPeer(RateLimitedServer::Loopback);

    // Three times what used to start the global backoff, from the first spelling of loopback and then two others
    host.FailHellos(10);
    host.SetPeer(PeerAddress::IPv4(127, 3, 3, 3));
    host.FailHellos(10);
    std::array<uint8_t, 16> ipv6Loopback{};
    ipv6Loopback[15] = 1;
    host.SetPeer(PeerAddress::IPv6(ipv6Loopback));
    host.FailHellos(10);

    // Nothing was refused at accept, and the right token gets in from each of them
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u);
    for (const PeerAddress &peer : {RateLimitedServer::Loopback, PeerAddress::IPv4(127, 3, 3, 3), PeerAddress::IPv6(ipv6Loopback)})
    {
        host.SetPeer(peer);
        const auto hello = host.Hello(Token);
        ASSERT_TRUE(hello.has_value()) << peer.ToString();
        EXPECT_EQ(hello->type, ServerInfoType) << peer.ToString() << ": " << hello->Text();
    }
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u);
}

TEST(RateLimitServerTest, AnIPv6PeerIsBackedOffLikeAnyAddress)
{
    LogCapture log;
    RateLimitedServer host;
    ASSERT_TRUE(host.started);
    host.SetPeer(PeerAddress::IPv6({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));

    host.FailHellos(3);
    EXPECT_TRUE(host.ClosedAtAccept());
    ASSERT_TRUE(PumpUntilLogged(host.server, log, "Rate limit: peer=2001:db8::1 is backed off"));
    EXPECT_TRUE(log.Has("peer=2001:db8::1"));
}

// ==================== Logs ====================

TEST(RateLimitServerTest, OneLineStartsABanNotOnePerRefusedConnectionAndNeverATokenOrAHash)
{
    LogCapture log;
    {
        RateLimitedServer host;
        ASSERT_TRUE(host.started);

        host.FailHellos(3);
        for (int i = 0; i < 25; ++i)
        {
            ASSERT_TRUE(host.ClosedAtAccept()) << i;
        }
        EXPECT_EQ(host.server.RateLimitedConnections(), 25u);
        ASSERT_TRUE(PumpUntilLogged(host.server, log, "Rate limit: peer=203.0.113.7 is backed off for 1000 ms after 3 failed Hellos"));

        // The backoff ends: the first connection after it reports how many were closed, once
        host.Advance(std::chrono::milliseconds(1000));
        const auto hello = host.Hello(Token);
        ASSERT_TRUE(hello.has_value());
        ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
        ASSERT_TRUE(PumpUntilLogged(host.server, log, "may connect again; 25 connection(s) were closed during its backoff"));
        host.server.ProcessCommands(std::chrono::milliseconds(0));
    }

    EXPECT_EQ(log.Count("is backed off for"), 1u) << "one line for the ban, however many connections it refused";
    EXPECT_EQ(log.Count("may connect again"), 1u);
    // 25 refused connections wrote no "Editor connected", "Waiting" or any other line of their own
    EXPECT_LE(log.Count("Editor connected"), 5u);
    for (const std::string &line : log.Lines())
    {
        EXPECT_EQ(line.find(Token), std::string::npos) << line;
        EXPECT_EQ(line.find("wrong-token"), std::string::npos) << line;
    }
}

TEST(RateLimitServerTest, HelloLinesNameThePeer)
{
    LogCapture log;
    RateLimitedServer host;
    ASSERT_TRUE(host.started);

    host.FailHellos(1);
    const auto hello = host.Hello(Token);
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();
    const auto mismatch = host.Hello(Token, std::nullopt, "999.0.0");
    ASSERT_TRUE(mismatch.has_value());
    ASSERT_TRUE(PumpUntilLogged(host.server, log, "Editor disconnected peer=203.0.113.7"));

    EXPECT_TRUE(log.Has("Refused Hello from editor client 'rate limit test' (user=- role=none): wrong access token peer=203.0.113.7"));
    EXPECT_TRUE(log.Has("said Hello (protocol " + std::string{ProtocolVersion} + ") user=token role=admin peer=203.0.113.7"));
    EXPECT_TRUE(log.Has("Protocol version mismatch"));
    EXPECT_TRUE(log.Has("Editor connected peer=203.0.113.7"));
}

// ==================== Off, and nothing to limit ====================

TEST(RateLimitServerTest, ARateLimitingHostWithoutAnAccessTokenHasNoFailedHellosAndNoLimiter)
{
    RateLimitedServer host(RateLimitPolicy{}, false);
    ASSERT_TRUE(host.started);
    EXPECT_FALSE(host.server.IsRateLimiting());

    // Any token is accepted, from any address, for as long as anyone likes
    for (int i = 0; i < 6; ++i)
    {
        const auto hello = host.Hello("whatever");
        ASSERT_TRUE(hello.has_value()) << i;
        EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
    }
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u);
}

TEST(RateLimitServerTest, TurningItOffLeavesFailedHellosUncounted)
{
    EditorServer probe;
    ASSERT_TRUE(probe.SetAccessToken(Token));
    EXPECT_TRUE(probe.IsRateLimiting()) << "on by default with an access token";
    ASSERT_TRUE(probe.SetRateLimiting(false));
    EXPECT_FALSE(probe.IsRateLimiting());

    RateLimitedServer host(RateLimitPolicy{}, true, false);
    ASSERT_TRUE(host.started);
    EXPECT_FALSE(host.server.IsRateLimiting());

    host.FailHellos(8);
    EXPECT_EQ(host.server.RateLimitedConnections(), 0u);
    const auto hello = host.Hello(Token);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
}

TEST(RateLimitServerTest, TheSettingsCantChangeWhileServing)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);
    EXPECT_FALSE(host.server.SetRateLimiting(false));
    EXPECT_FALSE(host.server.SetRateLimitPolicy(RateLimitPolicy{}));
    EXPECT_FALSE(host.server.SetRateLimitClock({}));
    EXPECT_FALSE(host.server.SetPeerAddressProvider({}));
    EXPECT_TRUE(host.server.IsRateLimiting());
}

TEST(RateLimitServerTest, ARestartBeginsWithEmptyTables)
{
    RateLimitedServer host;
    ASSERT_TRUE(host.started);
    host.FailHellos(3);
    ASSERT_TRUE(host.ClosedAtAccept());

    StopWithin(host.server, std::chrono::seconds(10));
    ASSERT_TRUE(host.server.Start(0));
    const auto hello = host.Hello(Token);
    ASSERT_TRUE(hello.has_value());
    EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
}

TEST(RateLimitServerTest, WithoutAProviderTheRealPeerIsRead)
{
    // getpeername on a loopback connection: 127.0.0.1, so the loopback rule applies, and the address lines say so
    LogCapture log;
    EditorServer server;
    ASSERT_TRUE(server.SetAccessToken(Token));
    ASSERT_TRUE(server.Start(0));
    {
        TestClient client(server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto hello = Roundtrip(server, client, CommandType::Hello, HelloPayload(Token));
        ASSERT_TRUE(hello.has_value());
        EXPECT_EQ(hello->type, ServerInfoType) << hello->Text();
        EXPECT_TRUE(PumpUntilLogged(server, log, "Editor connected peer=127.0.0.1"));
    }
    StopWithin(server, std::chrono::seconds(10));
}

// ==================== Per user (a tokens file needs the TLS library's SHA-256) ====================

namespace
{
    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _path = fs::temp_directory_path() /
                    ("n2-ratelimit-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_path);
        }

        ~TempFolder()
        {
            std::error_code error;
            fs::remove_all(_path, error);
        }

        TempFolder(const TempFolder &) = delete;
        TempFolder &operator=(const TempFolder &) = delete;

        [[nodiscard]] const fs::path &Path() const { return _path; }

    private:
        fs::path _path;
    };

    struct User
    {
        std::string name;
        std::string token;
        TokenRole role;
    };

    const User Alice{"alice", "alice-secret-token-0001", TokenRole::Admin};
    const User Bob{"bob", "bob-secret-token-0002", TokenRole::Editor};

    void WriteTokensFile(const fs::path &file, const std::vector<User> &users)
    {
        std::string text = "[";
        for (size_t i = 0; i < users.size(); ++i)
        {
            const auto hash = HashToken(users[i].token);
            ASSERT_TRUE(hash.has_value());
            text += (i == 0 ? "" : ",\n ") + std::string("{\"user\":\"") + users[i].name + "\",\"role\":\"" +
                    std::string(TokenRoleName(users[i].role)) + "\",\"hash\":\"" + FormatTokenHash(*hash) + "\"}";
        }
        text += "]\n";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        }
#ifndef _WIN32
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
    }

    class UserRateLimitTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            if (!EditorTlsServer::IsAvailable())
            {
                GTEST_SKIP() << "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";
            }
            _folder = std::make_unique<TempFolder>();
            _file = _folder->Path() / "tokens.json";
            WriteTokensFile(_file, {Alice, Bob});

            ASSERT_TRUE(_server.SetTokensFile(_file));
            ASSERT_TRUE(_server.SetHelloTimeout(std::chrono::seconds(30)));
            ASSERT_TRUE(_server.SetRateLimitClock([this] { return RateLimiter::TimePoint(std::chrono::milliseconds(_nowMs)); }));
            ASSERT_TRUE(_server.SetPeerAddressProvider([this](int) { return _peer.Get(); }));
            ASSERT_TRUE(_server.Start(0));
        }

        void TearDown() override
        {
            if (_server.IsRunning())
                StopWithin(_server, std::chrono::seconds(10));
        }

        std::optional<Frame> Hello(const User &user, const std::string &token)
        {
            TestClient client(_server.GetPort());
            if (!client.IsConnected())
                return std::nullopt;
            return Roundtrip(_server, client, CommandType::Hello, HelloPayload(token, std::string{ProtocolVersion}, user.name));
        }

        std::unique_ptr<TempFolder> _folder;
        fs::path _file;
        EditorServer _server;
        std::atomic<int64_t> _nowMs{1000};
        SharedPeer _peer{PeerAddress::IPv4(127, 0, 0, 1)};
    };
}

TEST_F(UserRateLimitTest, FailuresNamingAUserBackThatUserOffFromAnywhereAndOthersAreServed)
{
    LogCapture log;

    // Behind a tunnel (every peer 127.0.0.1) someone guesses alice's token three times
    for (int i = 0; i < 3; ++i)
    {
        const auto response = Hello(Alice, "not-alices-token");
        ASSERT_TRUE(response.has_value()) << i;
        EXPECT_EQ(response->type, ErrorType) << i;
    }

    // A wrong token naming alice is turned away for now: the same Error, a closed connection (and, as the token is
    // checked first, it is counted as a further failure: UserRateLimitTest.TheTokenIsCheckedBeforeTheUsersBackoff)
    {
        TestClient client(_server.GetPort());
        ASSERT_TRUE(client.IsConnected());
        const auto response = Roundtrip(_server, client, CommandType::Hello,
                                        HelloPayload("still-not-alices-token", std::string{ProtocolVersion}, Alice.name));
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->type, ErrorType);
        EXPECT_EQ(response->Text(), EditorServer::RateLimitedError);
        EXPECT_TRUE(client.IsClosedByServer());
    }
    // ... and so from another address
    _peer.Set(PeerAddress::IPv4(198, 51, 100, 20));
    const auto elsewhere = Hello(Alice, "not-alices-token-either");
    ASSERT_TRUE(elsewhere.has_value());
    EXPECT_EQ(elsewhere->Text(), EditorServer::RateLimitedError);

    // Bob, through the same tunnel, is not: the loopback is not backed off (three failures), only alice
    _peer.Set(PeerAddress::IPv4(127, 0, 0, 1));
    const auto bob = Hello(Bob, Bob.token);
    ASSERT_TRUE(bob.has_value());
    EXPECT_EQ(bob->type, ServerInfoType) << bob->Text();
    EXPECT_EQ(_server.RateLimitedConnections(), 0u) << "a user's backoff refuses a Hello, not a connection at accept";

    // The backoff grows with the counted failures (5 by now: 4 s), and it does not keep alice out: she presents her
    // own token, which is checked first, and her success clears the count
    const auto alice = Hello(Alice, Alice.token);
    ASSERT_TRUE(alice.has_value());
    EXPECT_EQ(alice->type, ServerInfoType) << alice->Text();

    EXPECT_TRUE(PumpUntilLogged(_server, log, "Rate limit: user=alice is backed off for 1000 ms after 3 failed Hellos"));
    EXPECT_EQ(log.Count("Rate limit: user=alice"), 3u) << "one line per backoff that starts: 3, 4 and 5 failures";
    for (const std::string &line : log.Lines())
    {
        EXPECT_EQ(line.find(Alice.token), std::string::npos) << line;
        EXPECT_EQ(line.find("not-alices-token"), std::string::npos) << line;
    }
}

TEST_F(UserRateLimitTest, TheTokenIsCheckedBeforeTheUsersBackoff)
{
    LogCapture log;
    for (int i = 0; i < 3; ++i)
    {
        const auto response = Hello(Alice, "not-alices-token");
        ASSERT_TRUE(response.has_value()) << i;
        EXPECT_EQ(response->Text(), "Invalid access token") << i;
    }
    ASSERT_TRUE(PumpUntilLogged(_server, log, "Rate limit: user=alice is backed off for 1000 ms after 3 failed Hellos"));

    // A fourth wrong token during the backoff is refused as before, AND counted: the backoff doubles
    const auto fourth = Hello(Alice, "not-alices-token");
    ASSERT_TRUE(fourth.has_value());
    EXPECT_EQ(fourth->Text(), EditorServer::RateLimitedError);
    EXPECT_TRUE(PumpUntilLogged(_server, log, "Rate limit: user=alice is backed off for 2000 ms after 4 failed Hellos"));

    // A second on, the first backoff would have ended but the doubled one has not: a wrong token is still refused and
    // counted (5 failures: 4 s) ...
    _nowMs += 1000;
    const auto fifth = Hello(Alice, "not-alices-token");
    ASSERT_TRUE(fifth.has_value());
    EXPECT_EQ(fifth->Text(), EditorServer::RateLimitedError);
    EXPECT_TRUE(PumpUntilLogged(_server, log, "Rate limit: user=alice is backed off for 4000 ms after 5 failed Hellos"));

    // ... while the correct token gets in during the backoff, from any address, and clears it
    _peer.Set(PeerAddress::IPv4(198, 51, 100, 20));
    const auto right = Hello(Alice, Alice.token);
    ASSERT_TRUE(right.has_value());
    EXPECT_EQ(right->type, ServerInfoType) << right->Text();

    // Cleared: a wrong token is an ordinary first failure again, answered as one
    const auto afterwards = Hello(Alice, "not-alices-token");
    ASSERT_TRUE(afterwards.has_value());
    EXPECT_EQ(afterwards->Text(), "Invalid access token");

    for (const std::string &line : log.Lines())
    {
        EXPECT_EQ(line.find(Alice.token), std::string::npos) << line;
        EXPECT_EQ(line.find("not-alices-token"), std::string::npos) << line;
    }
}

TEST_F(UserRateLimitTest, AuditLinesNameThePeer)
{
    LogCapture log;
    TestClient client(_server.GetPort());
    ASSERT_TRUE(client.IsConnected());
    const auto hello =
        Roundtrip(_server, client, CommandType::Hello, HelloPayload(Bob.token, std::string{ProtocolVersion}, Bob.name));
    ASSERT_TRUE(hello.has_value());
    ASSERT_EQ(hello->type, ServerInfoType) << hello->Text();

    BufferWriter w;
    w.WriteString("Cube");
    (void)Roundtrip(_server, client, CommandType::CreateEntity, w.Release()); // "No scene loaded": the audit line is first
    EXPECT_TRUE(log.Has("audit: user=bob role=editor command=CreateEntity peer=127.0.0.1"));
    EXPECT_TRUE(log.Has("user=bob role=editor peer=127.0.0.1"));
}
