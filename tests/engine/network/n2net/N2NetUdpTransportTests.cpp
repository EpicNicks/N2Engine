#include <gtest/gtest.h>

#include "engine/network/n2net/UdpTransport.hpp"

// UdpTransport over the loopback interface (net-a4): two real transports exchanging A3 channel traffic with and without
// simulated loss, the fail-closed bind rule, the peer cap, unknown sources, truncated and oversize datagrams, IPv6,
// re-binding and the Windows connection-reset pitfall. Native only: the web build has no sockets, so there this file is
// empty. Every wait polls against a deadline (no fixed sleeps), so a slow machine only makes a test slower, and a hang
// fails at the deadline instead of stalling CI.

#if N2NET_HAS_UDP

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "N2NetTestUtil.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

namespace
{
    constexpr int kWaitMs = 20'000;

    std::uint64_t NowMicros()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

    void PauseBriefly()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    /// A plain UDP socket the tests use as "some other host": bound to loopback on an ephemeral port, non-blocking
    class RawUdp
    {
    public:
        explicit RawUdp(bool v6 = false) : v6_(v6)
        {
#ifdef _WIN32
            WSADATA data;
            started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            if (!started_)
            {
                return;
            }
#endif
            socket_ = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket_ == kInvalid)
            {
                return;
            }
#ifdef _WIN32
            u_long enable = 1;
            ioctlsocket(socket_, FIONBIO, &enable);
#else
            fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL, 0) | O_NONBLOCK);
#endif
            sockaddr_storage storage{};
            int length = 0;
            Fill(storage, length, v6 ? "::1" : "127.0.0.1", 0);
            if (::bind(socket_, reinterpret_cast<const sockaddr *>(&storage), length) != 0)
            {
                Close();
                return;
            }
            sockaddr_storage actual{};
#ifdef _WIN32
            int actualLength = static_cast<int>(sizeof actual);
#else
            socklen_t actualLength = sizeof actual;
#endif
            if (getsockname(socket_, reinterpret_cast<sockaddr *>(&actual), &actualLength) != 0)
            {
                Close();
                return;
            }
            port_ = v6 ? ntohs(reinterpret_cast<sockaddr_in6 &>(actual).sin6_port)
                       : ntohs(reinterpret_cast<sockaddr_in &>(actual).sin_port);
        }

        ~RawUdp()
        {
            Close();
#ifdef _WIN32
            if (started_)
            {
                WSACleanup();
            }
#endif
        }

        RawUdp(const RawUdp &) = delete;
        RawUdp &operator=(const RawUdp &) = delete;

        [[nodiscard]] bool Ok() const { return socket_ != kInvalid; }
        [[nodiscard]] std::uint16_t Port() const { return port_; }

        /// Closes the socket now, so its port is closed (a send to it draws an ICMP port unreachable)
        void Close()
        {
            if (socket_ != kInvalid)
            {
#ifdef _WIN32
                closesocket(socket_);
#else
                close(socket_);
#endif
                socket_ = kInvalid;
            }
        }

        bool SendTo(const char *host, std::uint16_t port, const std::vector<std::uint8_t> &bytes)
        {
            sockaddr_storage storage{};
            int length = 0;
            if (!Fill(storage, length, host, port))
            {
                return false;
            }
            const auto sent = sendto(socket_, reinterpret_cast<const char *>(bytes.data()),
#ifdef _WIN32
                                     static_cast<int>(bytes.size()),
#else
                                     bytes.size(),
#endif
                                     0, reinterpret_cast<const sockaddr *>(&storage),
#ifdef _WIN32
                                     length);
#else
                                     static_cast<socklen_t>(length));
#endif
            return sent >= 0 && static_cast<std::size_t>(sent) == bytes.size();
        }

        bool SendToLoopback(std::uint16_t port, const std::vector<std::uint8_t> &bytes)
        {
            return SendTo(v6_ ? "::1" : "127.0.0.1", port, bytes);
        }

        /// The next datagram, waiting up to `timeoutMs`; nothing if none came
        std::optional<std::vector<std::uint8_t>> Receive(int timeoutMs)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            std::vector<std::uint8_t> buffer(70000);
            for (;;)
            {
                const auto received = recvfrom(socket_, reinterpret_cast<char *>(buffer.data()),
#ifdef _WIN32
                                               static_cast<int>(buffer.size()),
#else
                                               buffer.size(),
#endif
                                               0, nullptr, nullptr);
                if (received >= 0)
                {
                    buffer.resize(static_cast<std::size_t>(received));
                    return buffer;
                }
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    return std::nullopt;
                }
                PauseBriefly();
            }
        }

    private:
#ifdef _WIN32
        using Handle = SOCKET;
        static constexpr Handle kInvalid = INVALID_SOCKET;
        bool started_ = false;
#else
        using Handle = int;
        static constexpr Handle kInvalid = -1;
#endif

        bool Fill(sockaddr_storage &storage, int &length, const char *host, std::uint16_t port) const
        {
            std::memset(&storage, 0, sizeof storage);
            if (v6_)
            {
                auto &in6 = reinterpret_cast<sockaddr_in6 &>(storage);
                in6.sin6_family = AF_INET6;
                in6.sin6_port = htons(port);
                length = static_cast<int>(sizeof in6);
                return inet_pton(AF_INET6, host, &in6.sin6_addr) == 1;
            }
            auto &in = reinterpret_cast<sockaddr_in &>(storage);
            in.sin_family = AF_INET;
            in.sin_port = htons(port);
            length = static_cast<int>(sizeof in);
            return inet_pton(AF_INET, host, &in.sin_addr) == 1;
        }

        Handle socket_ = kInvalid;
        bool v6_ = false;
        std::uint16_t port_ = 0;
    };

    struct Event
    {
        TransportEventType type = TransportEventType::Datagram;
        PeerId peer = kInvalidPeer;
        std::vector<std::uint8_t> data;
    };

    std::vector<Event> PollAll(UdpTransport &transport)
    {
        std::vector<Event> events;
        transport.Poll(
            [&](const TransportEvent &event)
            {
                Event copy;
                copy.type = event.type;
                copy.peer = event.peer;
                copy.data.assign(event.data.begin(), event.data.end());
                events.push_back(std::move(copy));
            });
        return events;
    }

    std::size_t CountOf(const std::vector<Event> &events, TransportEventType type)
    {
        return static_cast<std::size_t>(
            std::count_if(events.begin(), events.end(), [type](const Event &event) { return event.type == type; }));
    }

    /// Polls into `events` until `done()` or the deadline; true if done
    bool WaitFor(UdpTransport &transport, std::vector<Event> &events, const std::function<bool()> &done,
                 int timeoutMs = kWaitMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            for (Event &event : PollAll(transport))
            {
                events.push_back(std::move(event));
            }
            if (done())
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            PauseBriefly();
        }
    }

    bool WaitForDatagrams(UdpTransport &transport, std::vector<Event> &events, std::size_t count,
                          int timeoutMs = kWaitMs)
    {
        return WaitFor(
            transport, events, [&] { return CountOf(events, TransportEventType::Datagram) >= count; }, timeoutMs);
    }

    std::unique_ptr<UdpTransport> OpenOrFail(const UdpTransportOptions &options = {})
    {
        UdpOpenResult result = UdpTransport::Open(options, false);
        EXPECT_EQ(result.error, UdpError::None) << result.message;
        return std::move(result.transport);
    }

    /// A transport that loses a seeded share of the datagrams passing through it, in both directions: the "simulated
    /// loss" over real sockets (loopback itself loses nothing)
    class LossyTransport final : public ITransport
    {
    public:
        LossyTransport(ITransport &inner, std::uint64_t seed, std::uint32_t lossPpm)
            : inner_(inner), random_(seed), lossPpm_(lossPpm)
        {
        }

        [[nodiscard]] std::size_t MaxPacketSize() const noexcept override { return inner_.MaxPacketSize(); }
        [[nodiscard]] bool IsReliableStream() const noexcept override { return inner_.IsReliableStream(); }

        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> datagram) override
        {
            if (Lose())
            {
                ++droppedOut;
                return TransportSendResult::Sent;
            }
            return inner_.Send(peer, datagram);
        }

        void Poll(EventHandler handler) override
        {
            inner_.Poll(
                [&](const TransportEvent &event)
                {
                    if (event.type == TransportEventType::Datagram && Lose())
                    {
                        ++droppedIn;
                        return;
                    }
                    handler(event);
                });
        }

        void Disconnect(PeerId peer) override { inner_.Disconnect(peer); }

        std::uint64_t droppedOut = 0;
        std::uint64_t droppedIn = 0;

    private:
        bool Lose() { return random_.NextBelow(1'000'000) < lossPpm_; }

        ITransport &inner_;
        SimRandom random_;
        std::uint32_t lossPpm_;
    };

    /// One end of a session: a Connection over some ITransport
    struct Side
    {
        ITransport *transport = nullptr;
        PeerId peer = kInvalidPeer;
        std::unique_ptr<Connection> connection;
        std::vector<ReceivedMessage> received;
    };

    ConnectionConfig FastConfig()
    {
        ConnectionConfig config = TestConfig();
        config.initialRtoMicros = 20'000;
        config.minRtoMicros = 10'000;
        config.maxRtoMicros = 200'000;
        config.keepAliveMicros = 100'000;
        config.idleTimeoutMicros = 60'000'000;
        config.reassemblyTimeoutMicros = 60'000'000;
        return config;
    }

    void Pump(Side &side)
    {
        const std::uint64_t now = NowMicros();
        side.transport->Poll(
            [&](const TransportEvent &event)
            {
                if (event.type == TransportEventType::PeerConnected)
                {
                    if (side.peer == kInvalidPeer)
                    {
                        side.peer = event.peer;
                    }
                    return;
                }
                if (event.type != TransportEventType::Datagram || event.peer != side.peer)
                {
                    return;
                }
                side.connection->OnPacket(now, event.data,
                                          [&](MessageChannel channel, std::span<const std::uint8_t> message)
                                          {
                                              ReceivedMessage received;
                                              received.channel = channel;
                                              received.data.assign(message.begin(), message.end());
                                              received.at = now;
                                              side.received.push_back(std::move(received));
                                          });
            });
        if (side.peer != kInvalidPeer)
        {
            const PeerId peer = side.peer;
            ITransport *transport = side.transport;
            side.connection->Update(NowMicros(),
                                    [&](std::span<const std::uint8_t> packet) { transport->Send(peer, packet); });
        }
    }

    constexpr std::uint32_t kClientReliable = 120;
    constexpr std::uint32_t kLargeIndex = 50;
    constexpr std::size_t kLargeSize = 40'000;
    constexpr std::uint32_t kServerReliable = 40;
    constexpr std::uint32_t kUnreliableBase = 1'000'000;
    constexpr std::uint32_t kUnreliableCount = 40;

    std::size_t ClientSize(std::uint32_t i)
    {
        return i == kLargeIndex ? kLargeSize : 1 + (i * 2654435761u >> 7) % 1500u;
    }

    std::size_t ServerSize(std::uint32_t i)
    {
        return 1 + (i * 40503u >> 3) % 1100u;
    }

    std::vector<const ReceivedMessage *> OfChannel(const Side &side, MessageChannel channel)
    {
        std::vector<const ReceivedMessage *> result;
        for (const ReceivedMessage &message : side.received)
        {
            if (message.channel == channel)
            {
                result.push_back(&message);
            }
        }
        return result;
    }

    /// Reliable-ordered (including one fragmented 40,000 byte message) and unreliable-sequenced traffic between two
    /// real transports, optionally through a lossy shim on each side
    void RunSession(std::uint32_t lossPpm)
    {
        std::unique_ptr<UdpTransport> serverUdp = OpenOrFail();
        UdpTransportOptions clientOptions;
        clientOptions.acceptUnknownPeers = false;
        std::unique_ptr<UdpTransport> clientUdp = OpenOrFail(clientOptions);
        ASSERT_NE(serverUdp, nullptr);
        ASSERT_NE(clientUdp, nullptr);

        LossyTransport serverLossy(*serverUdp, 11, lossPpm);
        LossyTransport clientLossy(*clientUdp, 22, lossPpm);
        Side server;
        Side client;
        server.transport = lossPpm != 0 ? static_cast<ITransport *>(&serverLossy) : serverUdp.get();
        client.transport = lossPpm != 0 ? static_cast<ITransport *>(&clientLossy) : clientUdp.get();
        server.connection = std::make_unique<Connection>(FastConfig(), NowMicros());
        client.connection = std::make_unique<Connection>(FastConfig(), NowMicros());

        const UdpConnectResult connected = clientUdp->Connect("127.0.0.1", serverUdp->LocalPort());
        ASSERT_EQ(connected.error, UdpError::None) << connected.message;
        client.peer = connected.peer;

        for (std::uint32_t i = 0; i < kClientReliable; ++i)
        {
            ASSERT_EQ(client.connection->SendReliable(MakeMessage(i, ClientSize(i))), SendResult::Ok);
        }
        bool serverQueued = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            Pump(client);
            Pump(server);
            if (!serverQueued && server.peer != kInvalidPeer)
            {
                serverQueued = true;
                for (std::uint32_t i = 0; i < kServerReliable; ++i)
                {
                    ASSERT_EQ(server.connection->SendReliable(MakeMessage(i + 5000, ServerSize(i))), SendResult::Ok);
                }
            }
            if (OfChannel(server, MessageChannel::ReliableOrdered).size() >= kClientReliable &&
                OfChannel(client, MessageChannel::ReliableOrdered).size() >= kServerReliable)
            {
                done = true;
                break;
            }
            PauseBriefly();
        }
        ASSERT_TRUE(done) << "session did not finish within " << kWaitMs << " ms (server got "
                          << OfChannel(server, MessageChannel::ReliableOrdered).size() << ", client got "
                          << OfChannel(client, MessageChannel::ReliableOrdered).size() << ")";

        // Unreliable-sequenced messages go in a second phase: they only ride in packets with room left over, so sent
        // together with a full reliable queue they would wait for the end
        for (std::uint32_t j = 0; j < kUnreliableCount; ++j)
        {
            ASSERT_EQ(client.connection->SendUnreliable(MakeMessage(kUnreliableBase + j, 8 + j % 40)), SendResult::Ok);
        }
        const auto phaseTwoDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        int extraPumps = -1;
        while (std::chrono::steady_clock::now() < phaseTwoDeadline && extraPumps < 20)
        {
            Pump(client);
            Pump(server);
            if (extraPumps >= 0 || !OfChannel(server, MessageChannel::UnreliableSequenced).empty())
            {
                ++extraPumps;
            }
            PauseBriefly();
        }

        // Reliable-ordered: every message, whole, in order (including the fragmented one)
        const auto atServer = OfChannel(server, MessageChannel::ReliableOrdered);
        ASSERT_EQ(atServer.size(), kClientReliable);
        for (std::uint32_t i = 0; i < kClientReliable; ++i)
        {
            ASSERT_EQ(atServer[i]->data, MakeMessage(i, ClientSize(i))) << "client message " << i;
        }
        ASSERT_EQ(atServer[kLargeIndex]->data.size(), kLargeSize);
        const auto atClient = OfChannel(client, MessageChannel::ReliableOrdered);
        ASSERT_EQ(atClient.size(), kServerReliable);
        for (std::uint32_t i = 0; i < kServerReliable; ++i)
        {
            ASSERT_EQ(atClient[i]->data, MakeMessage(i + 5000, ServerSize(i))) << "server message " << i;
        }

        // Unreliable-sequenced: what arrives is intact and never goes backwards (loss may remove some)
        const auto unreliable = OfChannel(server, MessageChannel::UnreliableSequenced);
        std::uint32_t previous = 0;
        for (const ReceivedMessage *message : unreliable)
        {
            const std::uint32_t index = MessageIndex(message->data);
            ASSERT_GE(index, kUnreliableBase);
            ASSERT_LT(index, kUnreliableBase + kUnreliableCount);
            ASSERT_EQ(message->data, MakeMessage(index, 8 + (index - kUnreliableBase) % 40));
            ASSERT_GT(index, previous);
            previous = index;
        }
        if (lossPpm == 0)
        {
            EXPECT_FALSE(unreliable.empty());
        }

        if (lossPpm != 0)
        {
            // The shim really did drop datagrams, and the reliable channel still delivered everything
            EXPECT_GT(clientLossy.droppedOut + serverLossy.droppedOut + clientLossy.droppedIn + serverLossy.droppedIn,
                      0u);
            EXPECT_GT(client.connection->Stats().unitsResent + server.connection->Stats().unitsResent, 0u);
        }
        EXPECT_EQ(client.connection->IsFailed(), false);
        EXPECT_EQ(server.connection->IsFailed(), false);
    }

    std::vector<std::uint8_t> Bytes(std::size_t size, std::uint8_t fill)
    {
        return std::vector<std::uint8_t>(size, fill);
    }
} // namespace

TEST(N2NetUdpTransportTest, OpensOnLoopbackWithAnEphemeralPort)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    ASSERT_NE(transport, nullptr);
    EXPECT_NE(transport->LocalPort(), 0u);
    EXPECT_FALSE(transport->IsIpv6());
    EXPECT_EQ(transport->LocalAddress(), "127.0.0.1");
    EXPECT_EQ(transport->MaxPacketSize(), kMaxPacketSize);
    EXPECT_FALSE(transport->IsReliableStream());
    EXPECT_EQ(transport->PeerCount(), 0u);
    EXPECT_FALSE(transport->InsecureOverrideUsed());
    // Nothing waiting: Poll returns at once without blocking
    EXPECT_TRUE(PollAll(*transport).empty());
}

TEST(N2NetUdpTransportTest, ChannelsOverLoopbackWithoutLoss)
{
    RunSession(0);
}

TEST(N2NetUdpTransportTest, ChannelsOverLoopbackWithSimulatedLoss)
{
    RunSession(150'000);
}

TEST(N2NetUdpTransportTest, SendRejectsBadArguments)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    RawUdp other;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(other.Ok());
    const UdpConnectResult connected = transport->Connect("127.0.0.1", other.Port());
    ASSERT_EQ(connected.error, UdpError::None) << connected.message;

    EXPECT_EQ(transport->Send(kInvalidPeer, Bytes(10, 1)), TransportSendResult::UnknownPeer);
    EXPECT_EQ(transport->Send(connected.peer + 99, Bytes(10, 1)), TransportSendResult::UnknownPeer);
    EXPECT_EQ(transport->Send(connected.peer, Bytes(0, 1)), TransportSendResult::EmptyPacket);
    EXPECT_EQ(transport->Send(connected.peer, Bytes(kMaxPacketSize + 1, 1)), TransportSendResult::PacketTooLarge);
    EXPECT_EQ(transport->Send(connected.peer, Bytes(kMaxPacketSize, 7)), TransportSendResult::Sent);
    const auto arrived = other.Receive(kWaitMs);
    ASSERT_TRUE(arrived.has_value());
    EXPECT_EQ(*arrived, Bytes(kMaxPacketSize, 7));
}

TEST(N2NetUdpTransportTest, NonLoopbackBindIsRefusedWithoutEncryptionOrTheFlag)
{
    // Every one of these is refused before a socket is made, so none of them needs to exist on this machine
    for (const char *address : {"0.0.0.0", "::", "0:0:0:0:0:0:0:0", "10.1.2.3", "8.8.8.8", "169.254.1.1", "128.0.0.1",
                                "::2", "::ffff:8.8.8.8", "::ffff:0.0.0.0"})
    {
        UdpTransportOptions options;
        options.bindAddress = address;
        const UdpOpenResult result = UdpTransport::Open(options, false);
        EXPECT_EQ(result.error, UdpError::NonLoopbackRefused) << address;
        EXPECT_EQ(result.transport, nullptr) << address;
        EXPECT_FALSE(result.message.empty()) << address;
    }
}

TEST(N2NetUdpTransportTest, HostNamesAndOddSpellingsAreNeverTreatedAsLoopback)
{
    const std::string embeddedNul("127.0.0.1\0x", 11);
    for (const std::string &address : std::vector<std::string>{"localhost", "", " ", "127.0.0.1 ", " 127.0.0.1",
                                                              "example.com", "fe80::1%lo", "2130706433", "::1%1",
                                                              "[::1]", "127.0.0.1:80", embeddedNul})
    {
        UdpTransportOptions options;
        options.bindAddress = address;
        const UdpOpenResult result = UdpTransport::Open(options, false);
        EXPECT_NE(result.error, UdpError::None) << "'" << address << "'";
        EXPECT_EQ(result.transport, nullptr) << "'" << address << "'";
        // And the same with the override: a name is not an address at all
        options.insecureBind = true;
        const UdpOpenResult overridden = UdpTransport::Open(options, false);
        EXPECT_EQ(overridden.error, UdpError::InvalidAddress) << "'" << address << "'";
        EXPECT_FALSE(IsLoopbackAddressText(address)) << "'" << address << "'";
    }
}

TEST(N2NetUdpTransportTest, LoopbackSpellings)
{
    for (const char *address : {"127.0.0.1", "127.255.255.254", "127.0.0.2", "::1", "0:0:0:0:0:0:0:1", "::ffff:127.0.0.1"})
    {
        EXPECT_TRUE(IsLoopbackAddressText(address)) << address;
    }
    for (const char *address : {"0.0.0.0", "::", "126.255.255.255", "128.0.0.0", "::2", "::ffff:128.0.0.1", "1::1"})
    {
        EXPECT_FALSE(IsLoopbackAddressText(address)) << address;
    }
}

TEST(N2NetUdpTransportTest, InsecureBindOverridesWithOneLoudWarning)
{
    std::vector<std::string> warnings;
    UdpTransportOptions options;
    options.bindAddress = "0.0.0.0";
    options.insecureBind = true;
    options.warningSink = [&warnings](std::string_view text) { warnings.emplace_back(text); };
    UdpOpenResult result = UdpTransport::Open(options, false);
    ASSERT_EQ(result.error, UdpError::None) << result.message;
    ASSERT_NE(result.transport, nullptr);
    EXPECT_TRUE(result.transport->InsecureOverrideUsed());
    EXPECT_NE(result.transport->LocalPort(), 0u);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("WARNING"), std::string::npos);
    EXPECT_NE(warnings[0].find("0.0.0.0"), std::string::npos);
    EXPECT_NE(warnings[0].find("NO encryption"), std::string::npos);
    // The warning is once per transport: further use does not repeat it
    PollAll(*result.transport);
    EXPECT_EQ(warnings.size(), 1u);
}

TEST(N2NetUdpTransportTest, EncryptionAvailableAllowsANonLoopbackBindWithoutTheFlagOrAWarning)
{
    std::vector<std::string> warnings;
    UdpTransportOptions options;
    options.bindAddress = "0.0.0.0";
    options.warningSink = [&warnings](std::string_view text) { warnings.emplace_back(text); };
    const UdpOpenResult result = UdpTransport::Open(options, true);
    ASSERT_EQ(result.error, UdpError::None) << result.message;
    EXPECT_FALSE(result.transport->InsecureOverrideUsed());
    EXPECT_TRUE(warnings.empty());
}

TEST(N2NetUdpTransportTest, TheFlagOnALoopbackBindIsQuiet)
{
    std::vector<std::string> warnings;
    UdpTransportOptions options;
    options.insecureBind = true;
    options.warningSink = [&warnings](std::string_view text) { warnings.emplace_back(text); };
    const UdpOpenResult result = UdpTransport::Open(options, false);
    ASSERT_EQ(result.error, UdpError::None) << result.message;
    EXPECT_FALSE(result.transport->InsecureOverrideUsed());
    EXPECT_TRUE(warnings.empty());
}

TEST(N2NetUdpTransportTest, ConnectToANonLoopbackAddressFollowsTheSameRule)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    ASSERT_NE(transport, nullptr);
    const UdpConnectResult refused = transport->Connect("8.8.8.8", 4000);
    EXPECT_EQ(refused.error, UdpError::NonLoopbackRefused);
    EXPECT_EQ(refused.peer, kInvalidPeer);
    EXPECT_EQ(transport->PeerCount(), 0u);
    EXPECT_TRUE(PollAll(*transport).empty());
}

TEST(N2NetUdpTransportTest, ConnectValidation)
{
    UdpTransportOptions options;
    options.maxPeers = 2;
    std::unique_ptr<UdpTransport> transport = OpenOrFail(options);
    ASSERT_NE(transport, nullptr);

    EXPECT_EQ(transport->Connect("localhost", 4000).error, UdpError::InvalidAddress);
    EXPECT_EQ(transport->Connect("", 4000).error, UdpError::InvalidAddress);
    EXPECT_EQ(transport->Connect("0.0.0.0", 4000).error, UdpError::InvalidAddress);
    EXPECT_EQ(transport->Connect("224.0.0.1", 4000).error, UdpError::InvalidAddress);
    EXPECT_EQ(transport->Connect("255.255.255.255", 4000).error, UdpError::InvalidAddress);
    EXPECT_EQ(transport->Connect("127.0.0.1", 0).error, UdpError::InvalidPort);
    EXPECT_EQ(transport->Connect("::1", 4000).error, UdpError::FamilyMismatch);

    const UdpConnectResult first = transport->Connect("127.0.0.1", 4001);
    ASSERT_EQ(first.error, UdpError::None) << first.message;
    EXPECT_NE(first.peer, kInvalidPeer);
    EXPECT_EQ(transport->Connect("127.0.0.1", 4001).error, UdpError::AlreadyConnected);
    EXPECT_EQ(transport->Connect("127.0.0.1", 4002).error, UdpError::None);
    EXPECT_EQ(transport->Connect("127.0.0.1", 4003).error, UdpError::TooManyPeers);
    EXPECT_EQ(transport->PeerCount(), 2u);

    // Connect announces the peers on the next Poll, once each, and sends nothing
    const std::vector<Event> events = PollAll(*transport);
    EXPECT_EQ(CountOf(events, TransportEventType::PeerConnected), 2u);
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(transport->Stats().datagramsSent, 0u);
}

TEST(N2NetUdpTransportTest, FirstDatagramFromAnUnknownSourceMakesAPeer)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    RawUdp a;
    RawUdp b;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(a.Ok());
    ASSERT_TRUE(b.Ok());
    ASSERT_NE(a.Port(), b.Port());

    ASSERT_TRUE(a.SendToLoopback(transport->LocalPort(), Bytes(20, 0xA1)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, TransportEventType::PeerConnected);
    EXPECT_EQ(events[1].type, TransportEventType::Datagram);
    EXPECT_EQ(events[0].peer, events[1].peer);
    EXPECT_NE(events[1].peer, kInvalidPeer);
    EXPECT_EQ(events[1].data, Bytes(20, 0xA1));
    const PeerId peerA = events[1].peer;

    // The same source again is the same peer, with no second connect event
    events.clear();
    ASSERT_TRUE(a.SendToLoopback(transport->LocalPort(), Bytes(5, 0xA2)));
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].peer, peerA);

    // Another source (same address, other port) is another peer: a datagram is never attributed by what it claims
    events.clear();
    ASSERT_TRUE(b.SendToLoopback(transport->LocalPort(), Bytes(5, 0xA2)));
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, TransportEventType::PeerConnected);
    EXPECT_NE(events[1].peer, peerA);
    const PeerId peerB = events[1].peer;
    EXPECT_EQ(transport->PeerCount(), 2u);

    // The transport has sent nothing of its own to either source
    EXPECT_FALSE(a.Receive(50).has_value());
    EXPECT_FALSE(b.Receive(50).has_value());
    EXPECT_EQ(transport->Stats().datagramsSent, 0u);

    // Only Send replies, and only to the peer named
    ASSERT_EQ(transport->Send(peerB, Bytes(9, 0xBB)), TransportSendResult::Sent);
    const auto reply = b.Receive(kWaitMs);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(*reply, Bytes(9, 0xBB));
    EXPECT_FALSE(a.Receive(50).has_value());

    UdpPeerInfo info;
    ASSERT_TRUE(transport->GetPeerInfo(peerA, info));
    EXPECT_EQ(info.address, "127.0.0.1");
    EXPECT_EQ(info.port, a.Port());
    EXPECT_EQ(info.datagramsReceived, 2u);
    EXPECT_EQ(info.bytesReceived, 25u);
    EXPECT_EQ(info.bytesSent, 0u);
    ASSERT_TRUE(transport->GetPeerInfo(peerB, info));
    EXPECT_EQ(info.bytesSent, 9u);
    EXPECT_FALSE(transport->GetPeerInfo(kInvalidPeer, info));
    EXPECT_EQ(transport->Stats().peersAccepted, 2u);
}

TEST(N2NetUdpTransportTest, PeerCapDropsSourcesBeyondIt)
{
    UdpTransportOptions options;
    options.maxPeers = 3;
    std::unique_ptr<UdpTransport> transport = OpenOrFail(options);
    ASSERT_NE(transport, nullptr);
    std::vector<std::unique_ptr<RawUdp>> raws;
    for (int i = 0; i < 6; ++i)
    {
        raws.push_back(std::make_unique<RawUdp>());
        ASSERT_TRUE(raws.back()->Ok());
    }
    // One at a time, so which three get in is known: the first three
    std::vector<Event> events;
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_TRUE(raws[i]->SendToLoopback(transport->LocalPort(), Bytes(4, static_cast<std::uint8_t>(i))));
        ASSERT_TRUE(WaitForDatagrams(*transport, events, static_cast<std::size_t>(i) + 1));
    }
    EXPECT_EQ(transport->PeerCount(), 3u);
    for (int i = 3; i < 6; ++i)
    {
        ASSERT_TRUE(raws[i]->SendToLoopback(transport->LocalPort(), Bytes(4, static_cast<std::uint8_t>(i))));
        ASSERT_TRUE(
            WaitFor(*transport, events, [&] { return transport->Stats().droppedPeerLimit >= static_cast<std::uint64_t>(i - 2); }));
    }
    EXPECT_EQ(transport->PeerCount(), 3u);
    EXPECT_EQ(transport->Stats().droppedPeerLimit, 3u);
    EXPECT_EQ(CountOf(events, TransportEventType::Datagram), 3u);
    EXPECT_EQ(CountOf(events, TransportEventType::PeerConnected), 3u);

    // A peer already in keeps working while the table is full
    ASSERT_TRUE(raws[0]->SendToLoopback(transport->LocalPort(), Bytes(4, 0x55)));
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 4));
    EXPECT_EQ(events.back().data, Bytes(4, 0x55));
    EXPECT_EQ(CountOf(events, TransportEventType::PeerConnected), 3u);
}

TEST(N2NetUdpTransportTest, ClientModeDropsEverythingNotFromItsPeer)
{
    RawUdp server;
    RawUdp stranger;
    ASSERT_TRUE(server.Ok());
    ASSERT_TRUE(stranger.Ok());
    UdpTransportOptions options;
    options.acceptUnknownPeers = false;
    std::unique_ptr<UdpTransport> client = OpenOrFail(options);
    ASSERT_NE(client, nullptr);
    const UdpConnectResult connected = client->Connect("127.0.0.1", server.Port());
    ASSERT_EQ(connected.error, UdpError::None) << connected.message;

    // The stranger "claims" to be the server's traffic: it is dropped, and answered with nothing
    ASSERT_TRUE(stranger.SendToLoopback(client->LocalPort(), Bytes(30, 0xEE)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitFor(*client, events, [&] { return client->Stats().droppedUnknownSource >= 1; }));
    EXPECT_EQ(CountOf(events, TransportEventType::Datagram), 0u);
    EXPECT_EQ(client->PeerCount(), 1u);
    EXPECT_FALSE(stranger.Receive(50).has_value());

    // The real peer's datagram is delivered, attributed to the connected peer
    ASSERT_TRUE(server.SendToLoopback(client->LocalPort(), Bytes(8, 0x11)));
    ASSERT_TRUE(WaitForDatagrams(*client, events, 1));
    const auto datagram = std::find_if(events.begin(), events.end(),
                                       [](const Event &event) { return event.type == TransportEventType::Datagram; });
    ASSERT_NE(datagram, events.end());
    EXPECT_EQ(datagram->peer, connected.peer);
    EXPECT_EQ(datagram->data, Bytes(8, 0x11));
    EXPECT_EQ(client->Stats().peersAccepted, 0u);
}

TEST(N2NetUdpTransportTest, EmptyTruncatedAndOversizeDatagrams)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    RawUdp raw;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(raw.Ok());
    const std::uint16_t port = transport->LocalPort();

    ASSERT_TRUE(raw.SendToLoopback(port, Bytes(0, 0)));        // empty
    ASSERT_TRUE(raw.SendToLoopback(port, Bytes(1201, 0x42)));  // one over
    ASSERT_TRUE(raw.SendToLoopback(port, Bytes(65'000, 0x42))); // far over
    ASSERT_TRUE(raw.SendToLoopback(port, Bytes(1200, 0x43)));  // exactly the maximum: delivered
    ASSERT_TRUE(raw.SendToLoopback(port, Bytes(3, 0x44)));     // shorter than any n2net header: delivered as is

    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 2));
    ASSERT_TRUE(WaitFor(*transport, events,
                        [&]
                        {
                            const UdpTransportStats &stats = transport->Stats();
                            return stats.droppedEmpty >= 1 && stats.droppedOversize >= 2;
                        }));
    EXPECT_EQ(transport->Stats().droppedEmpty, 1u);
    EXPECT_EQ(transport->Stats().droppedOversize, 2u);
    ASSERT_EQ(CountOf(events, TransportEventType::Datagram), 2u);
    std::vector<const Event *> datagrams;
    for (const Event &event : events)
    {
        if (event.type == TransportEventType::Datagram)
        {
            datagrams.push_back(&event);
        }
    }
    EXPECT_EQ(datagrams[0]->data, Bytes(1200, 0x43));
    EXPECT_EQ(datagrams[1]->data, Bytes(3, 0x44));
    // The dropped ones made no peer of their own: one source, one peer
    EXPECT_EQ(transport->PeerCount(), 1u);

    // What the transport delivers as is, the packet layer then rejects (it never crashes or applies it)
    Connection connection(FastConfig(), 0);
    for (const Event *datagram : datagrams)
    {
        EXPECT_NE(connection.OnPacket(1, datagram->data, [](MessageChannel, std::span<const std::uint8_t>) {}),
                  PacketStatus::Ok);
    }
    EXPECT_EQ(connection.Stats().packetsRejected, 2u);
}

TEST(N2NetUdpTransportTest, IdlePeersAreDropped)
{
    UdpTransportOptions options;
    options.peerIdleTimeoutMs = 60;
    std::unique_ptr<UdpTransport> transport = OpenOrFail(options);
    RawUdp raw;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(raw.Ok());

    ASSERT_TRUE(raw.SendToLoopback(transport->LocalPort(), Bytes(4, 1)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    const PeerId first = events.back().peer;
    ASSERT_TRUE(WaitFor(*transport, events, [&] { return CountOf(events, TransportEventType::PeerDisconnected) >= 1; }));
    EXPECT_EQ(events.back().type, TransportEventType::PeerDisconnected);
    EXPECT_EQ(events.back().peer, first);
    EXPECT_EQ(transport->PeerCount(), 0u);
    EXPECT_EQ(transport->Stats().peersTimedOut, 1u);
    EXPECT_EQ(transport->Send(first, Bytes(4, 1)), TransportSendResult::UnknownPeer);

    // The same source later is a new peer
    events.clear();
    ASSERT_TRUE(raw.SendToLoopback(transport->LocalPort(), Bytes(4, 2)));
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    EXPECT_NE(events.back().peer, first);
}

TEST(N2NetUdpTransportTest, DisconnectForgetsThePeerLocallyAndSendsNothing)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    RawUdp raw;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(raw.Ok());
    ASSERT_TRUE(raw.SendToLoopback(transport->LocalPort(), Bytes(4, 1)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    const PeerId peer = events.back().peer;

    transport->Disconnect(peer);
    EXPECT_EQ(transport->PeerCount(), 0u);
    EXPECT_EQ(transport->Send(peer, Bytes(4, 1)), TransportSendResult::UnknownPeer);
    EXPECT_EQ(transport->Stats().datagramsSent, 0u);
    EXPECT_FALSE(raw.Receive(50).has_value());
    transport->Disconnect(peer);          // already gone: a no-op
    transport->Disconnect(kInvalidPeer);  // never valid: a no-op
    EXPECT_TRUE(PollAll(*transport).empty());

    events.clear();
    ASSERT_TRUE(raw.SendToLoopback(transport->LocalPort(), Bytes(4, 2)));
    ASSERT_TRUE(WaitForDatagrams(*transport, events, 1));
    EXPECT_NE(events.back().peer, peer);
}

TEST(N2NetUdpTransportTest, PollReadsAtMostTheConfiguredNumberOfDatagrams)
{
    UdpTransportOptions options;
    options.maxDatagramsPerPoll = 10;
    std::unique_ptr<UdpTransport> transport = OpenOrFail(options);
    RawUdp raw;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(raw.Ok());
    for (std::uint8_t i = 0; i < 50; ++i)
    {
        ASSERT_TRUE(raw.SendToLoopback(transport->LocalPort(), Bytes(4, i)));
    }
    std::size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (total < 50 && std::chrono::steady_clock::now() < deadline)
    {
        const std::size_t got = CountOf(PollAll(*transport), TransportEventType::Datagram);
        EXPECT_LE(got, 10u);
        total += got;
        PauseBriefly();
    }
    EXPECT_EQ(total, 50u);
}

TEST(N2NetUdpTransportTest, ConnectAndDisconnectInsideThePollHandlerAreHarmless)
{
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    RawUdp raw;
    ASSERT_NE(transport, nullptr);
    ASSERT_TRUE(raw.Ok());
    ASSERT_TRUE(raw.SendToLoopback(transport->LocalPort(), Bytes(4, 1)));

    bool sawDatagram = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (!sawDatagram && std::chrono::steady_clock::now() < deadline)
    {
        transport->Poll(
            [&](const TransportEvent &event)
            {
                if (event.type != TransportEventType::Datagram)
                {
                    return;
                }
                sawDatagram = true;
                EXPECT_EQ(transport->Connect("127.0.0.1", 4000).error, UdpError::Busy);
                transport->Disconnect(event.peer); // forbidden by the interface; ignored
                EXPECT_EQ(transport->Send(event.peer, Bytes(3, 9)), TransportSendResult::Sent);
            });
        PauseBriefly();
    }
    ASSERT_TRUE(sawDatagram);
    EXPECT_EQ(transport->PeerCount(), 1u);
    const auto reply = raw.Receive(kWaitMs);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(*reply, Bytes(3, 9));
}

TEST(N2NetUdpTransportTest, ShutdownAndRebindTheSamePort)
{
    std::unique_ptr<UdpTransport> first = OpenOrFail();
    ASSERT_NE(first, nullptr);
    const std::uint16_t port = first->LocalPort();

    // While it is open nobody else may take the port (no address reuse)
    UdpTransportOptions same;
    same.port = port;
    const UdpOpenResult busy = UdpTransport::Open(same, false);
    EXPECT_EQ(busy.error, UdpError::BindFailed);
    EXPECT_EQ(busy.transport, nullptr);
    EXPECT_NE(busy.systemError, 0);

    first.reset();
    const UdpOpenResult second = UdpTransport::Open(same, false);
    ASSERT_EQ(second.error, UdpError::None) << second.message;
    EXPECT_EQ(second.transport->LocalPort(), port);

    // And it works: a datagram to the port arrives at the new transport
    RawUdp raw;
    ASSERT_TRUE(raw.Ok());
    ASSERT_TRUE(raw.SendToLoopback(port, Bytes(6, 3)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*second.transport, events, 1));
}

TEST(N2NetUdpTransportTest, SendsToAClosedPortDoNotEndTheReceiveLoop)
{
    // The Windows UDP pitfall: a send to a closed port draws an ICMP port unreachable that, by default, makes the next
    // recvfrom fail with WSAECONNRESET and hides the datagrams behind it. The transport turns that off (and ignores it
    // if it still arrives). Elsewhere the same sequence must simply work.
    std::unique_ptr<UdpTransport> transport = OpenOrFail();
    ASSERT_NE(transport, nullptr);
    std::uint16_t deadPort = 0;
    {
        RawUdp dead;
        ASSERT_TRUE(dead.Ok());
        deadPort = dead.Port();
    } // closed here
    const UdpConnectResult connected = transport->Connect("127.0.0.1", deadPort);
    ASSERT_EQ(connected.error, UdpError::None) << connected.message;
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_EQ(transport->Send(connected.peer, Bytes(16, 1)), TransportSendResult::Sent);
    }

    RawUdp live;
    ASSERT_TRUE(live.Ok());
    ASSERT_TRUE(live.SendToLoopback(transport->LocalPort(), Bytes(12, 0x77)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitFor(*transport, events,
                        [&]
                        {
                            return std::any_of(events.begin(), events.end(), [](const Event &event)
                                               { return event.type == TransportEventType::Datagram &&
                                                        event.data == Bytes(12, 0x77); });
                        }));
    EXPECT_EQ(transport->Stats().receiveErrors, 0u);
    EXPECT_EQ(transport->Stats().sendErrors, 0u);
}

TEST(N2NetUdpTransportTest, Ipv6Loopback)
{
    UdpTransportOptions options;
    options.bindAddress = "::1";
    const UdpOpenResult opened = UdpTransport::Open(options, false);
    if (opened.error != UdpError::None)
    {
        GTEST_SKIP() << "no IPv6 loopback here: " << opened.message << " (system error " << opened.systemError << ")";
    }
    ASSERT_TRUE(opened.transport->IsIpv6());
    EXPECT_NE(opened.transport->LocalPort(), 0u);

    RawUdp raw(true);
    if (!raw.Ok())
    {
        GTEST_SKIP() << "could not make an IPv6 test socket";
    }
    ASSERT_TRUE(raw.SendToLoopback(opened.transport->LocalPort(), Bytes(14, 0x61)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*opened.transport, events, 1));
    const PeerId peer = events.back().peer;
    EXPECT_EQ(events.back().data, Bytes(14, 0x61));
    UdpPeerInfo info;
    ASSERT_TRUE(opened.transport->GetPeerInfo(peer, info));
    EXPECT_EQ(info.address, "::1");
    EXPECT_EQ(info.port, raw.Port());

    ASSERT_EQ(opened.transport->Send(peer, Bytes(5, 0x62)), TransportSendResult::Sent);
    const auto reply = raw.Receive(kWaitMs);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(*reply, Bytes(5, 0x62));

    // An IPv4 destination is not reachable from an IPv6 loopback socket
    EXPECT_EQ(opened.transport->Connect("127.0.0.1", 4000).error == UdpError::None,
              opened.transport->DualStack());
}

TEST(N2NetUdpTransportTest, DualStackSocketServesIpv4PeersAsIpv4)
{
    UdpTransportOptions options;
    options.bindAddress = "::";
    options.insecureBind = true; // "::" is not loopback; this socket is closed again at the end of the test
    options.warningSink = [](std::string_view) {};
    const UdpOpenResult opened = UdpTransport::Open(options, false);
    if (opened.error != UdpError::None)
    {
        GTEST_SKIP() << "no IPv6 here: " << opened.message << " (system error " << opened.systemError << ")";
    }
    if (!opened.transport->DualStack())
    {
        GTEST_SKIP() << "the system did not accept a dual-stack IPv6 socket";
    }
    RawUdp raw4;
    ASSERT_TRUE(raw4.Ok());
    ASSERT_TRUE(raw4.SendToLoopback(opened.transport->LocalPort(), Bytes(10, 0x41)));
    std::vector<Event> events;
    ASSERT_TRUE(WaitForDatagrams(*opened.transport, events, 1));
    const PeerId peer = events.back().peer;
    UdpPeerInfo info;
    ASSERT_TRUE(opened.transport->GetPeerInfo(peer, info));
    EXPECT_EQ(info.address, "127.0.0.1");
    ASSERT_EQ(opened.transport->Send(peer, Bytes(7, 0x42)), TransportSendResult::Sent);
    const auto reply = raw4.Receive(kWaitMs);
    ASSERT_TRUE(reply.has_value());
    EXPECT_EQ(*reply, Bytes(7, 0x42));

    // The v6-only setting is honoured too
    UdpTransportOptions only6 = options;
    only6.dualStack = false;
    const UdpOpenResult v6only = UdpTransport::Open(only6, false);
    ASSERT_EQ(v6only.error, UdpError::None) << v6only.message;
    EXPECT_FALSE(v6only.transport->DualStack());
    EXPECT_EQ(v6only.transport->Connect("127.0.0.1", 4000).error, UdpError::FamilyMismatch);
}

#endif // N2NET_HAS_UDP
