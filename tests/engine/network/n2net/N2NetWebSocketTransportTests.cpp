#include "engine/network/n2net/UdpTransport.hpp"
#include "engine/network/n2net/WebSocketTransport.hpp"
#include <gtest/gtest.h>
#if N2NET_HAS_UDP
#include <chrono>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
using namespace N2Engine::N2Net;
namespace
{
    struct Client
    {
#ifdef _WIN32
        SOCKET socket = INVALID_SOCKET;
        ~Client()
        {
            if (socket != INVALID_SOCKET)
                closesocket(socket);
        }
#else
        int socket = -1;
        ~Client()
        {
            if (socket >= 0)
                close(socket);
        }
#endif
        bool Open(std::uint16_t port)
        {
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
                return false;
            const int noDelay = 1;
            if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&noDelay),
                           sizeof(noDelay)) != 0)
                return false;
#ifdef _WIN32
            u_long nonblock = 1;
            return ioctlsocket(socket, FIONBIO, &nonblock) == 0;
#else
            return fcntl(socket, F_SETFL, O_NONBLOCK) == 0;
#endif
        }
        bool Write(std::span<const std::uint8_t> bytes)
        {
            return send(socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0) ==
                   static_cast<int>(bytes.size());
        }
        bool Http()
        {
            const std::string request = "GET /game HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: "
                                        "keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: "
                                        "n2net\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
            return Write({reinterpret_cast<const std::uint8_t *>(request.data()), request.size()});
        }
        std::vector<std::uint8_t> Read()
        {
            std::array<char, 8192> bytes{};
            const int size = static_cast<int>(recv(socket, bytes.data(), bytes.size(), 0));
            if (size <= 0)
                return {};
            return {bytes.begin(), bytes.begin() + size};
        }
    };
    std::vector<std::uint8_t> Mask(std::span<const std::uint8_t> data, std::uint8_t op = 0x82)
    {
        std::vector<std::uint8_t> b{op};
        if (data.size() < 126)
            b.push_back(static_cast<std::uint8_t>(0x80 | data.size()));
        else
        {
            b.push_back(0xfe);
            b.push_back(static_cast<std::uint8_t>(data.size() >> 8));
            b.push_back(static_cast<std::uint8_t>(data.size()));
        }
        b.insert(b.end(), {1, 2, 3, 4});
        for (std::size_t i = 0; i < data.size(); ++i)
            b.push_back(data[i] ^ static_cast<std::uint8_t>(1 + i % 4));
        return b;
    }
    std::unique_ptr<WebSocketTransport> Listen()
    {
        WebSocketListenOptions options;
        options.protocolId = 7;
        options.authenticate = [](std::string_view token) -> std::optional<JoinTokenEntry> {
            if (token != "secret")
                return {};
            JoinTokenEntry identity;
            identity.user = "tester";
            return identity;
        };
        std::string error;
        auto result = WebSocketTransport::Listen(std::move(options), error);
        EXPECT_TRUE(result) << error;
        return result;
    }
    const std::vector<std::uint8_t> auth{'N', '2', 'W', 'S', 7, 0, 0, 0, 's', 'e', 'c', 'r', 'e', 't'};
    void Pump(WebSocketTransport &transport, ITransport::EventHandler handler)
    {
        for (int i = 0; i < 30; ++i)
        {
            transport.Poll(handler);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
} // namespace
TEST(WebSocketTransportTest, UpgradeAuthenticatesThenPreservesBinaryMessageBoundaries)
{
    auto server = Listen();
    ASSERT_TRUE(server);
    Client client;
    ASSERT_TRUE(client.Open(server->LocalPort()));
    std::vector<TransportEventType> events;
    std::vector<std::vector<std::uint8_t>> received;
    PeerId peer = 0;
    auto handler = [&](const TransportEvent &e) {
        events.push_back(e.type);
        peer = e.peer;
        if (e.type == TransportEventType::Datagram)
            received.emplace_back(e.data.begin(), e.data.end());
    };
    ASSERT_TRUE(client.Http());
    Pump(*server, handler);
    EXPECT_TRUE(events.empty());
    const auto reply = client.Read();
    const std::string http(reply.begin(), reply.end());
    EXPECT_NE(http.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
    ASSERT_TRUE(client.Write(Mask(auth)));
    Pump(*server, handler);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0], TransportEventType::PeerConnected);
    ASSERT_TRUE(server->Identity(peer));
    EXPECT_EQ(server->Identity(peer)->user, "tester");
    std::array<std::uint8_t, 3> first{1, 0, 255}, second{7, 8, 9};
    ASSERT_TRUE(client.Write(Mask(first)));
    ASSERT_TRUE(client.Write(Mask(second)));
    Pump(*server, handler);
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0], (std::vector<std::uint8_t>{1, 0, 255}));
    EXPECT_EQ(received[1], (std::vector<std::uint8_t>{7, 8, 9}));
    EXPECT_TRUE(server->IsReliableStream());
    EXPECT_EQ(server->Send(peer, first), TransportSendResult::Sent);
    server->Disconnect(peer);
    EXPECT_EQ(server->Send(peer, first), TransportSendResult::UnknownPeer);
}
TEST(WebSocketTransportTest, FragmentationAndPingWorkButUnmaskedAndOversizedFramesDisconnect)
{
    auto server = Listen();
    ASSERT_TRUE(server);
    Client client;
    ASSERT_TRUE(client.Open(server->LocalPort()));
    int joins = 0, leaves = 0;
    std::vector<std::uint8_t> received;
    auto handler = [&](const TransportEvent &e) {
        if (e.type == TransportEventType::PeerConnected)
            ++joins;
        else if (e.type == TransportEventType::PeerDisconnected)
            ++leaves;
        else
            received.assign(e.data.begin(), e.data.end());
    };
    ASSERT_TRUE(client.Http());
    Pump(*server, handler);
    client.Read();
    ASSERT_TRUE(client.Write(Mask(auth)));
    Pump(*server, handler);
    client.Read();
    ASSERT_EQ(joins, 1);
    ASSERT_TRUE(client.Write(Mask(std::array<std::uint8_t, 2>{1, 2}, 0x02)));
    ASSERT_TRUE(client.Write(Mask(std::array<std::uint8_t, 1>{8}, 0x89)));
    ASSERT_TRUE(client.Write(Mask(std::array<std::uint8_t, 2>{3, 4}, 0x80)));
    Pump(*server, handler);
    EXPECT_EQ(received, (std::vector<std::uint8_t>{1, 2, 3, 4}));
    ASSERT_TRUE(client.Write(std::array<std::uint8_t, 3>{0x82, 1, 0}));
    Pump(*server, handler);
    EXPECT_EQ(leaves, 1);
}
TEST(WebSocketTransportTest, InvalidTokenNeverAdmitsAPeerAndPublicPlaintextBindIsRefused)
{
    auto server = Listen();
    ASSERT_TRUE(server);
    Client client;
    ASSERT_TRUE(client.Open(server->LocalPort()));
    int events = 0;
    auto handler = [&](const TransportEvent &) { ++events; };
    ASSERT_TRUE(client.Http());
    Pump(*server, handler);
    client.Read();
    auto wrong = auth;
    wrong.back() = 'x';
    ASSERT_TRUE(client.Write(Mask(wrong)));
    Pump(*server, handler);
    EXPECT_EQ(events, 0);
    WebSocketListenOptions options;
    options.bindAddress = "0.0.0.0";
    std::string error;
    EXPECT_FALSE(WebSocketTransport::Listen(options, error));
}

TEST(WebSocketTransportTest, AFullStreamQueueDisconnectsRatherThanLosingAnAcceptedDatagram)
{
    auto server=Listen();ASSERT_TRUE(server);
    Client client;ASSERT_TRUE(client.Open(server->LocalPort()));
    PeerId peer=0;int leaves=0;
    auto handler=[&](const TransportEvent &event) {
        if(event.type==TransportEventType::PeerConnected)peer=event.peer;
        if(event.type==TransportEventType::PeerDisconnected)++leaves;
    };
    ASSERT_TRUE(client.Http());Pump(*server,handler);client.Read();
    ASSERT_TRUE(client.Write(Mask(auth)));Pump(*server,handler);client.Read();ASSERT_NE(peer,0u);
    std::array<std::uint8_t,kMaxPacketSize> packet{};
    bool refused=false;
    for(int i=0;i<300;++i)
        if(server->Send(peer,packet)!=TransportSendResult::Sent) {refused=true;break;}
    EXPECT_TRUE(refused);
    Pump(*server,handler);EXPECT_EQ(leaves,1);
}

TEST(WebSocketTransportTest, OversizedFrameIsRejectedFromItsHeaderBeforePayloadAllocation)
{
    auto server=Listen();ASSERT_TRUE(server);Client client;ASSERT_TRUE(client.Open(server->LocalPort()));
    int joins=0,leaves=0;
    auto handler=[&](const TransportEvent &event) {
        if(event.type==TransportEventType::PeerConnected)++joins;
        if(event.type==TransportEventType::PeerDisconnected)++leaves;
    };
    ASSERT_TRUE(client.Http());Pump(*server,handler);client.Read();
    ASSERT_TRUE(client.Write(Mask(auth)));Pump(*server,handler);client.Read();ASSERT_EQ(joins,1);
    // Masked binary frame claiming 1201 bytes; no payload is sent.
    ASSERT_TRUE(client.Write(std::array<std::uint8_t,8>{0x82,0xfe,4,0xb1,1,2,3,4}));
    Pump(*server,handler);EXPECT_EQ(leaves,1);
}
#endif
