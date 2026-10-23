#include "engine/network/n2net/MultiplexTransport.hpp"
#include <deque>
#include <gtest/gtest.h>
using namespace N2Engine::N2Net;
namespace
{
    class Endpoint final : public ITransport
    {
      public:
        explicit Endpoint(bool stream) : stream(stream) {}
        std::size_t MaxPacketSize() const noexcept override
        {
            return stream ? kMaxPacketSize : 1000;
        }
        bool IsReliableStream() const noexcept override
        {
            return stream;
        }
        TransportSendResult Send(PeerId peer, std::span<const std::uint8_t> bytes) override
        {
            sentPeer = peer;
            sent.assign(bytes.begin(), bytes.end());
            return TransportSendResult::Sent;
        }
        void Poll(EventHandler handler) override
        {
            while (!events.empty())
            {
                const auto type = events.front();
                events.pop_front();
                handler(TransportEvent{type, 1, {}});
            }
        }
        void Disconnect(PeerId peer) override
        {
            disconnected = peer;
        }
        bool stream;
        PeerId sentPeer = 0, disconnected = 0;
        std::vector<std::uint8_t> sent;
        std::deque<TransportEventType> events;
    };
} // namespace
TEST(MultiplexTransportTest, EqualUnderlyingPeerIdsAreDistinctAndKeepTheirReliability)
{
    Endpoint udp(false), ws(true);
    MultiplexTransport mux({&udp, &ws});
    udp.events.push_back(TransportEventType::PeerConnected);
    ws.events.push_back(TransportEventType::PeerConnected);
    std::vector<PeerId> ids;
    auto handler = [&](const TransportEvent &e) { ids.push_back(e.peer); };
    mux.Poll(handler);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_NE(ids[0], ids[1]);
    EXPECT_FALSE(mux.IsReliablePeer(ids[0]));
    EXPECT_TRUE(mux.IsReliablePeer(ids[1]));
    EXPECT_EQ(mux.MaxPeerPacketSize(ids[0]),1000u);
    EXPECT_EQ(mux.MaxPeerPacketSize(ids[1]),kMaxPacketSize);
    const std::vector<std::uint8_t> packet{1, 2, 3};
    EXPECT_EQ(mux.Send(ids[1], packet), TransportSendResult::Sent);
    EXPECT_EQ(ws.sent, packet);
    EXPECT_TRUE(udp.sent.empty());
    ITransport *source = nullptr;
    PeerId original = 0;
    ASSERT_TRUE(mux.Source(ids[1], source, original));
    EXPECT_EQ(source, &ws);
    EXPECT_EQ(original, 1u);
    mux.Disconnect(ids[1]);
    EXPECT_EQ(ws.disconnected, 1u);
    EXPECT_EQ(mux.Send(ids[1], packet), TransportSendResult::UnknownPeer);
    EXPECT_EQ(mux.Send(ids[0], packet), TransportSendResult::Sent);
    udp.events.push_back(TransportEventType::PeerDisconnected);
    mux.Poll(handler);
    EXPECT_FALSE(mux.Source(ids[0], source, original));
}
