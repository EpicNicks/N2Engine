#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Packet.hpp"

// The packet layer (net-a3): sequence and ack wraparound, the reliable-ordered channel (exactly once and in order
// under loss, reordering and duplication), the unreliable-sequenced channel (newest wins), fragmentation and
// reassembly at every boundary size and out of order, the caps and timeouts, the RTT and loss statistics, and the
// stream-transport mode. Pairs of connections run over the SimNetwork; hostile and hand-made packets go straight in.

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

namespace
{
    using Packets = std::vector<std::vector<std::uint8_t>>;

    Packets Capture(Connection &connection, std::uint64_t now)
    {
        Packets out;
        connection.Update(now, [&](std::span<const std::uint8_t> packet) { out.emplace_back(packet.begin(), packet.end()); });
        return out;
    }

    PacketStatus Feed(Connection &connection, std::uint64_t now, std::span<const std::uint8_t> packet,
                      std::vector<ReceivedMessage> &received)
    {
        return connection.OnPacket(now, packet,
                                   [&](MessageChannel channel, std::span<const std::uint8_t> message)
                                   {
                                       ReceivedMessage item;
                                       item.channel = channel;
                                       item.data.assign(message.begin(), message.end());
                                       item.at = now;
                                       received.push_back(std::move(item));
                                   });
    }

    std::vector<ReceivedMessage> OfChannel(const std::vector<ReceivedMessage> &all, MessageChannel channel)
    {
        std::vector<ReceivedMessage> out;
        for (const ReceivedMessage &message : all)
        {
            if (message.channel == channel)
            {
                out.push_back(message);
            }
        }
        return out;
    }

    std::size_t SizeFor(std::uint32_t index)
    {
        // 1 byte to a few units, with the odd sizes in the mix
        switch (index % 7)
        {
        case 0:
            return 1;
        case 1:
            return 100 + (index * 13) % 400;
        case 2:
            return kMaxUnitPayload;
        case 3:
            return kMaxUnitPayload + 1;
        case 4:
            return 2 * kMaxUnitPayload;
        case 5:
            return 1 + (index * 97) % 3000;
        default:
            return 20;
        }
    }

    void ExpectInOrderExactlyOnce(const std::vector<ReceivedMessage> &received, std::uint32_t count,
                                  std::uint32_t firstIndex = 0)
    {
        const std::vector<ReceivedMessage> reliable = OfChannel(received, MessageChannel::ReliableOrdered);
        ASSERT_EQ(reliable.size(), count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uint32_t index = firstIndex + i;
            ASSERT_EQ(reliable[i].data, MakeMessage(index, SizeFor(index))) << "message " << i;
        }
    }

    SimLinkConfig HarshLink()
    {
        SimLinkConfig link;
        link.latencyMicros = 20'000;
        link.jitterMicros = 10'000;
        link.lossPpm = 300'000;
        link.reorderPpm = 100'000;
        link.reorderDelayMicros = 40'000;
        link.duplicatePpm = 100'000;
        return link;
    }

    void SendAll(Connection &connection, std::uint32_t count, std::uint32_t firstIndex = 0)
    {
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::uint32_t index = firstIndex + i;
            ASSERT_EQ(connection.SendReliable(MakeMessage(index, SizeFor(index))), SendResult::Ok) << index;
        }
    }
} // namespace

// ---- the reliable channel ----

TEST(N2NetConnectionTest, ReliableMessagesArriveInOrderOnAPerfectLink)
{
    SimLinkConfig link;
    link.latencyMicros = 15'000;
    TwoPeers peers(1, link);
    SendAll(*peers.peerA.connection, 300);
    ASSERT_TRUE(peers.RunUntil([&] { return peers.peerB.received.size() >= 300; }, 20'000'000));
    ExpectInOrderExactlyOnce(peers.peerB.received, 300);
    peers.RunFor(1'000'000);
    EXPECT_TRUE(peers.peerA.connection->ReliableIdle());
    EXPECT_EQ(peers.peerA.connection->Stats().unitsResent, 0u);
    EXPECT_EQ(peers.peerA.connection->Stats().packetsLost, 0u);
    EXPECT_EQ(peers.peerB.connection->Stats().unitsDuplicate, 0u);
    EXPECT_TRUE(peers.peerA.rejected.empty());
    EXPECT_TRUE(peers.peerB.rejected.empty());
}

TEST(N2NetConnectionTest, ReliableUnderThirtyPercentLossWithReorderingAndDuplication)
{
    TwoPeers peers(2026, HarshLink());
    constexpr std::uint32_t kCount = 1200;
    SendAll(*peers.peerA.connection, kCount);
    SendAll(*peers.peerB.connection, kCount, 5000);

    const std::size_t bound = peers.peerA.connection->MemoryBoundBytes();
    std::size_t peak = 0;
    const bool done = peers.RunUntil(
        [&]
        {
            peak = std::max({peak, peers.peerA.connection->MemoryInUseBytes(), peers.peerB.connection->MemoryInUseBytes()});
            return OfChannel(peers.peerB.received, MessageChannel::ReliableOrdered).size() >= kCount &&
                   OfChannel(peers.peerA.received, MessageChannel::ReliableOrdered).size() >= kCount;
        },
        600'000'000);
    ASSERT_TRUE(done);
    EXPECT_FALSE(peers.peerA.connection->IsFailed());
    EXPECT_FALSE(peers.peerB.connection->IsFailed());
    ExpectInOrderExactlyOnce(peers.peerB.received, kCount);
    ExpectInOrderExactlyOnce(peers.peerA.received, kCount, 5000);
    EXPECT_LE(peak, bound);

    // nothing more arrives later: exactly once
    peers.RunFor(5'000'000);
    EXPECT_EQ(OfChannel(peers.peerB.received, MessageChannel::ReliableOrdered).size(), kCount);
    EXPECT_EQ(OfChannel(peers.peerA.received, MessageChannel::ReliableOrdered).size(), kCount);

    // the impairments really happened and were absorbed
    const ConnectionStats &stats = peers.peerA.connection->Stats();
    EXPECT_GT(stats.unitsResent, 0u);
    EXPECT_GT(stats.packetsLost, 0u);
    EXPECT_GT(peers.peerB.connection->Stats().packetsDuplicate, 0u);
    EXPECT_GT(peers.peerB.connection->Stats().unitsDuplicate + peers.peerB.connection->Stats().packetsDuplicate, 0u);
}

TEST(N2NetConnectionTest, SequenceAndAckNumbersWrapAround)
{
    // Packet numbers and unit ids start 40 below 65536 and cross the wrap many times over 1000 messages
    ConnectionConfig config = TestConfig();
    config.initialSequence = 65496;
    config.initialUnitId = 65496;
    SimLinkConfig link;
    link.latencyMicros = 10'000;
    link.lossPpm = 150'000;
    link.reorderPpm = 80'000;
    link.reorderDelayMicros = 25'000;
    TwoPeers peers(5, link, config, config);
    SendAll(*peers.peerA.connection, 1000);
    SendAll(*peers.peerB.connection, 1000, 9000);
    ASSERT_TRUE(peers.RunUntil(
        [&]
        {
            return OfChannel(peers.peerB.received, MessageChannel::ReliableOrdered).size() >= 1000 &&
                   OfChannel(peers.peerA.received, MessageChannel::ReliableOrdered).size() >= 1000;
        },
        300'000'000));
    ExpectInOrderExactlyOnce(peers.peerB.received, 1000);
    ExpectInOrderExactlyOnce(peers.peerA.received, 1000, 9000);
    EXPECT_GT(peers.peerA.connection->Stats().packetsSent, 200u); // well past the wrap
    peers.RunFor(3'000'000);
    EXPECT_TRUE(peers.peerA.connection->ReliableIdle());
    EXPECT_TRUE(peers.peerB.connection->ReliableIdle());
}

TEST(N2NetConnectionTest, AckBitsDescribeTheReceivedPackets)
{
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    const std::array<std::uint8_t, 1> data{1};
    for (int value : {10, 12, 13})
    {
        const std::uint16_t sequence = static_cast<std::uint16_t>(value);
        ASSERT_EQ(Feed(receiver, 1000, PacketBuilder(sequence).Unreliable(sequence, data).Build(), received), PacketStatus::Ok);
    }
    const Packets out = Capture(receiver, 2000);
    ASSERT_EQ(out.size(), 1u);
    PacketHeader header;
    ByteReader reader(out[0]);
    ASSERT_TRUE(ReadHeader(reader, header));
    EXPECT_TRUE(header.hasAck);
    EXPECT_EQ(header.ack, 13);
    // bit 0 is packet 12, bit 1 is 11 (missing), bit 2 is 10
    EXPECT_EQ(header.ackBits, 0b101u);
    EXPECT_EQ(out[0].size(), kHeaderSize); // an ack-only packet has no blocks
}

TEST(N2NetConnectionTest, AckBitsAcrossTheWrap)
{
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    const std::array<std::uint8_t, 1> data{1};
    for (int value : {65534, 65535, 0, 1})
    {
        const std::uint16_t sequence = static_cast<std::uint16_t>(value);
        ASSERT_EQ(Feed(receiver, 1000, PacketBuilder(sequence).Unreliable(sequence, data).Build(), received), PacketStatus::Ok);
    }
    const Packets out = Capture(receiver, 2000);
    ASSERT_EQ(out.size(), 1u);
    PacketHeader header;
    ByteReader reader(out[0]);
    ASSERT_TRUE(ReadHeader(reader, header));
    EXPECT_EQ(header.ack, 1);
    EXPECT_EQ(header.ackBits, 0b111u); // packets 0, 65535 and 65534
}

TEST(N2NetConnectionTest, AnAckOfPacketsAcrossTheWrapReleasesTheirUnits)
{
    ConnectionConfig config = TestConfig();
    config.initialSequence = 65535;
    Connection sender(config, 0);
    const std::vector<std::uint8_t> message{1, 2, 3};
    ASSERT_EQ(sender.SendReliable(message), SendResult::Ok);
    ASSERT_EQ(Capture(sender, 1000).size(), 1u); // packet 65535
    ASSERT_EQ(sender.SendReliable(message), SendResult::Ok);
    ASSERT_EQ(Capture(sender, 2000).size(), 1u); // packet 0
    EXPECT_FALSE(sender.ReliableIdle());

    std::vector<ReceivedMessage> received;
    // ack 0 with bit 0 set: packets 0 and 65535 arrived
    ASSERT_EQ(Feed(sender, 3000, PacketBuilder(0).Ack(0, 1).Build(), received), PacketStatus::Ok);
    EXPECT_TRUE(sender.ReliableIdle());
    EXPECT_EQ(sender.Stats().packetsAcked, 2u);
    EXPECT_EQ(sender.Stats().packetsLost, 0u);
}

TEST(N2NetConnectionTest, AGapInTheAcksResendsEarlyWithoutWaitingForTheTimeout)
{
    Connection sender(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        const std::vector<std::uint8_t> message{i};
        ASSERT_EQ(sender.SendReliable(message), SendResult::Ok);
        ASSERT_EQ(Capture(sender, 1000 + i * 1000u).size(), 1u);
    }
    // the peer got packets 4 and 3 only
    ASSERT_EQ(Feed(sender, 6000, PacketBuilder(0).Ack(4, 0b1).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(sender.Stats().packetsAcked, 2u);
    EXPECT_EQ(sender.Stats().packetsLost, 2u); // 0 (4 behind) and 1 (3 behind); 2 is only 2 behind: not yet

    const Packets resend = Capture(sender, 7000); // far inside the 200 ms timeout
    ASSERT_EQ(resend.size(), 1u);
    EXPECT_EQ(sender.Stats().unitsResent, 2u);
    // the packet carries unit 0 and unit 1 again
    ByteReader reader(resend[0]);
    PacketHeader header;
    ASSERT_TRUE(ReadHeader(reader, header));
    PacketBlock block;
    std::vector<std::uint16_t> ids;
    while (ReadBlock(reader, block) == BlockResult::Block)
    {
        ids.push_back(block.id);
    }
    EXPECT_EQ(ids, (std::vector<std::uint16_t>{0, 1}));
}

TEST(N2NetConnectionTest, ATimedOutPacketIsResentWithBackoff)
{
    ConnectionConfig config = TestConfig();
    config.initialRtoMicros = 100'000;
    config.minRtoMicros = 100'000;
    config.maxRtoMicros = 2'000'000;
    Connection sender(config, 0);
    const std::vector<std::uint8_t> message{7};
    ASSERT_EQ(sender.SendReliable(message), SendResult::Ok);
    ASSERT_EQ(Capture(sender, 0).size(), 1u);
    EXPECT_EQ(Capture(sender, 99'999).size(), 0u);   // not yet
    EXPECT_EQ(Capture(sender, 100'000).size(), 1u);  // first resend at the RTO
    EXPECT_EQ(Capture(sender, 299'999).size(), 0u);  // the second wait is doubled: 200 ms
    EXPECT_EQ(Capture(sender, 300'000).size(), 1u);  // second resend
    EXPECT_EQ(Capture(sender, 699'999).size(), 0u);  // then 400 ms
    EXPECT_EQ(Capture(sender, 700'000).size(), 1u);
    EXPECT_EQ(sender.Stats().unitsResent, 3u);
}

// ---- the unreliable channel ----

TEST(N2NetConnectionTest, UnreliableNewestWinsWhateverTheArrivalOrder)
{
    Connection sender(TestConfig(), 0);
    Packets packets;
    for (std::uint8_t i = 0; i < 5; ++i)
    {
        const std::vector<std::uint8_t> message{i};
        ASSERT_EQ(sender.SendUnreliable(message), SendResult::Ok);
        const Packets out = Capture(sender, 1000u * (i + 1u));
        ASSERT_EQ(out.size(), 1u);
        packets.push_back(out[0]);
    }

    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    for (std::size_t index : {3u, 1u, 4u, 0u, 2u, 4u, 3u})
    {
        Feed(receiver, 10'000, packets[index], received);
    }
    // 3 arrives first; 1 is stale; 4 is newer; 0 and 2 are stale; the two repeats are stale (and also duplicate packets)
    ASSERT_EQ(received.size(), 2u);
    EXPECT_EQ(received[0].channel, MessageChannel::UnreliableSequenced);
    EXPECT_EQ(received[0].data, (std::vector<std::uint8_t>{3}));
    EXPECT_EQ(received[1].data, (std::vector<std::uint8_t>{4}));
    EXPECT_EQ(receiver.Stats().unreliableStale, 3u);
    EXPECT_EQ(receiver.Stats().packetsDuplicate, 2u);
}

TEST(N2NetConnectionTest, UnreliableSequenceWrapsAround)
{
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    const auto message = [](std::uint8_t tag) { return std::vector<std::uint8_t>{tag}; };
    std::uint16_t packet = 0;
    for (int value : {65534, 65535, 0, 1})
    {
        const std::uint16_t sequence = static_cast<std::uint16_t>(value);
        Feed(receiver, 1000, PacketBuilder(packet++).Unreliable(sequence, message(static_cast<std::uint8_t>(sequence & 0xFF))).Build(), received);
    }
    ASSERT_EQ(received.size(), 4u); // each is newer than the one before, across the wrap
    // 65535 now is older than 1
    Feed(receiver, 1000, PacketBuilder(packet++).Unreliable(65535, message(9)).Build(), received);
    EXPECT_EQ(received.size(), 4u);
    EXPECT_EQ(receiver.Stats().unreliableStale, 1u);
}

TEST(N2NetConnectionTest, UnreliableStreamOverALossyReorderingLinkOnlyMovesForward)
{
    SimLinkConfig link;
    link.latencyMicros = 20'000;
    link.jitterMicros = 15'000;
    link.lossPpm = 200'000;
    link.reorderPpm = 150'000;
    link.reorderDelayMicros = 60'000;
    link.duplicatePpm = 100'000;
    TwoPeers peers(88, link);
    for (std::uint32_t i = 0; i < 2000; ++i)
    {
        ASSERT_EQ(peers.peerA.connection->SendUnreliable(MakeMessage(i, 12)), SendResult::Ok);
        peers.Step(2000);
    }
    peers.RunFor(1'000'000);
    const std::vector<ReceivedMessage> got = OfChannel(peers.peerB.received, MessageChannel::UnreliableSequenced);
    ASSERT_GT(got.size(), 500u);
    ASSERT_LT(got.size(), 2000u); // some were lost, and nothing was resent
    for (std::size_t i = 1; i < got.size(); ++i)
    {
        ASSERT_GT(MessageIndex(got[i].data), MessageIndex(got[i - 1].data)) << i; // strictly forward: no duplicate, no stale
    }
    EXPECT_GT(peers.peerB.connection->Stats().unreliableStale, 0u);
    EXPECT_EQ(peers.peerA.connection->Stats().unitsSent, 0u);
}

TEST(N2NetConnectionTest, UnreliableQueueDropsTheOldestWhenFull)
{
    ConnectionConfig config = TestConfig();
    config.maxQueuedUnreliableMessages = 3;
    Connection sender(config, 0);
    for (std::uint8_t i = 0; i < 6; ++i)
    {
        ASSERT_EQ(sender.SendUnreliable(std::vector<std::uint8_t>{i}), SendResult::Ok);
    }
    EXPECT_EQ(sender.Stats().unreliableDropped, 3u);
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    for (const std::vector<std::uint8_t> &packet : Capture(sender, 1000))
    {
        Feed(receiver, 1000, packet, received);
    }
    ASSERT_EQ(received.size(), 3u);
    EXPECT_EQ(received[0].data, (std::vector<std::uint8_t>{3}));
    EXPECT_EQ(received[2].data, (std::vector<std::uint8_t>{5}));
}

// ---- fragmentation ----

TEST(N2NetConnectionTest, FragmentationAtEveryBoundarySize)
{
    const std::size_t cap = ConnectionConfig{}.maxMessageSize;
    const std::vector<std::size_t> sizes{1,
                                         2,
                                         kMaxUnitPayload - 1,
                                         kMaxUnitPayload,
                                         kMaxUnitPayload + 1,
                                         2 * kMaxUnitPayload - 1,
                                         2 * kMaxUnitPayload,
                                         2 * kMaxUnitPayload + 1,
                                         10 * kMaxUnitPayload,
                                         cap - kMaxUnitPayload,
                                         cap - 1,
                                         cap};
    std::uint32_t index = 0;
    for (std::size_t size : sizes)
    {
        SimLinkConfig link;
        link.latencyMicros = 5000;
        TwoPeers peers(100 + index, link);
        const std::vector<std::uint8_t> message = MakeMessage(index, size);
        ASSERT_EQ(peers.peerA.connection->SendReliable(message), SendResult::Ok) << size;
        ASSERT_TRUE(peers.RunUntil([&] { return !peers.peerB.received.empty(); }, 30'000'000)) << size;
        ASSERT_EQ(peers.peerB.received.size(), 1u) << size;
        EXPECT_EQ(peers.peerB.received[0].data, message) << size;
        peers.RunFor(300'000);
        EXPECT_EQ(peers.peerB.received.size(), 1u) << size;
        // exactly as many units as the size needs
        EXPECT_EQ(peers.peerA.connection->Stats().unitsSent, (size + kMaxUnitPayload - 1) / kMaxUnitPayload) << size;
        EXPECT_TRUE(peers.peerA.connection->ReliableIdle()) << size;
        ++index;
    }
}

TEST(N2NetConnectionTest, FragmentsArrivingOutOfOrderAndRepeatedAssembleOnce)
{
    Connection sender(TestConfig(), 0);
    const std::vector<std::uint8_t> message = MakeMessage(77, 30'000);
    ASSERT_EQ(sender.SendReliable(message), SendResult::Ok);
    Packets packets = Capture(sender, 1000);
    ASSERT_EQ(packets.size(), (30'000u + kMaxUnitPayload - 1) / kMaxUnitPayload); // one unit per packet

    // everything but the first unit, in reverse, with each repeated: nothing completes without unit 0
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    for (std::size_t i = packets.size() - 1; i >= 1; --i)
    {
        Feed(receiver, 2000, packets[i], received);
        Feed(receiver, 2000, packets[i], received); // a duplicate packet
    }
    EXPECT_TRUE(received.empty());
    EXPECT_GT(receiver.Stats().packetsDuplicate, 0u);
    // then the first unit completes it, once
    ASSERT_EQ(Feed(receiver, 3000, packets[0], received), PacketStatus::Ok);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].data, message);
    // and a late repeat of any fragment delivers nothing more
    Feed(receiver, 4000, packets[3], received);
    EXPECT_EQ(received.size(), 1u);
}

TEST(N2NetConnectionTest, ShuffledFragmentsOfManyMessagesStillComeOutInOrder)
{
    Connection sender(TestConfig(), 0);
    constexpr std::uint32_t kCount = 12;
    for (std::uint32_t i = 0; i < kCount; ++i)
    {
        ASSERT_EQ(sender.SendReliable(MakeMessage(i, SizeFor(i))), SendResult::Ok);
    }
    Packets packets = Capture(sender, 1000); // no acks come back: at most 32 packets, which is all of this
    ASSERT_GE(packets.size(), 4u);
    ASSERT_LE(packets.size(), 32u);

    // a deterministic shuffle
    SimRandom random(4242);
    for (std::size_t i = packets.size(); i > 1; --i)
    {
        std::swap(packets[i - 1], packets[random.NextBelow(static_cast<std::uint32_t>(i))]);
    }
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    for (const std::vector<std::uint8_t> &packet : packets)
    {
        Feed(receiver, 5000, packet, received);
    }
    ExpectInOrderExactlyOnce(received, kCount);
}

// ---- caps ----

TEST(N2NetConnectionTest, SendLimits)
{
    ConnectionConfig config = TestConfig();
    config.maxMessageSize = 5000;
    config.maxSendQueueBytes = 12'000;
    Connection connection(config, 0);
    EXPECT_EQ(connection.SendReliable({}), SendResult::Empty);
    EXPECT_EQ(connection.SendReliable(std::vector<std::uint8_t>(5001, 1)), SendResult::TooLarge);
    EXPECT_EQ(connection.SendReliable(std::vector<std::uint8_t>(5000, 1)), SendResult::Ok);
    EXPECT_EQ(connection.SendReliable(std::vector<std::uint8_t>(5000, 1)), SendResult::Ok);
    EXPECT_EQ(connection.SendReliable(std::vector<std::uint8_t>(2000, 1)), SendResult::Ok); // 12000: exactly full
    EXPECT_EQ(connection.SendReliable(std::vector<std::uint8_t>(1, 1)), SendResult::QueueFull);
    EXPECT_LE(connection.MemoryInUseBytes(), connection.MemoryBoundBytes());

    EXPECT_EQ(connection.SendUnreliable({}), SendResult::Empty);
    EXPECT_EQ(connection.SendUnreliable(std::vector<std::uint8_t>(kMaxUnreliablePayload + 1, 1)), SendResult::TooLarge);
    EXPECT_EQ(connection.SendUnreliable(std::vector<std::uint8_t>(kMaxUnreliablePayload, 1)), SendResult::Ok);

    // draining the queue makes room
    Capture(connection, 1000);
    EXPECT_EQ(connection.SendReliable(std::vector<std::uint8_t>(1, 1)), SendResult::Ok);
}

TEST(N2NetConnectionTest, ConfigIsClampedToTheHardCaps)
{
    ConnectionConfig config = TestConfig();
    config.maxMessageSize = 0xFFFF'FFFFu;
    config.reliableWindow = 60'000;
    config.maxSendQueueBytes = 0;
    Connection large(config, 0);
    EXPECT_EQ(large.Config().maxMessageSize, kHardMaxMessageSize);
    EXPECT_EQ(large.Config().reliableWindow, 1024u); // clamped, a power of two
    EXPECT_GE(large.Config().maxSendQueueBytes, large.Config().maxMessageSize);

    config.maxMessageSize = 0;
    config.reliableWindow = 0;
    Connection small(config, 0);
    EXPECT_EQ(small.Config().maxMessageSize, 1u);
    EXPECT_EQ(small.Config().reliableWindow, 1u);

    config.reliableWindow = 100;
    EXPECT_EQ(Connection(config, 0).Config().reliableWindow, 64u); // rounded down to a power of two

    // times are clamped so the backoff and the deadlines cannot wrap
    ConnectionConfig forever = TestConfig();
    forever.initialRtoMicros = ~0ull;
    forever.maxRtoMicros = ~0ull;
    forever.idleTimeoutMicros = ~0ull;
    Connection patient(forever, 0);
    EXPECT_EQ(patient.Config().maxRtoMicros, 1ull << 40);
    EXPECT_EQ(patient.Config().idleTimeoutMicros, 1ull << 40);
    ASSERT_EQ(patient.SendReliable(std::vector<std::uint8_t>{1}), SendResult::Ok);
    std::uint64_t now = 0;
    for (int i = 0; i < 12; ++i)
    {
        Capture(patient, now);
        now += 1ull << 34; // hours apart: the deadlines are far off and nothing wraps
    }
    EXPECT_FALSE(patient.IsFailed());
}

TEST(N2NetConnectionTest, TheWindowBoundsUnitsInFlight)
{
    ConnectionConfig config = TestConfig();
    config.reliableWindow = 8;
    SimLinkConfig link;
    link.latencyMicros = 20'000;
    TwoPeers peers(3, link, config, config);
    const std::vector<std::uint8_t> message = MakeMessage(1, 64 * 1024);
    ASSERT_EQ(peers.peerA.connection->SendReliable(message), SendResult::Ok);
    peers.Step(1000);
    EXPECT_EQ(peers.peerA.connection->Stats().unitsSent, 8u); // not 56: the window
    ASSERT_TRUE(peers.RunUntil([&] { return !peers.peerB.received.empty(); }, 60'000'000));
    EXPECT_EQ(peers.peerB.received[0].data, message);
    EXPECT_LE(peers.peerA.connection->MemoryInUseBytes(), peers.peerA.connection->MemoryBoundBytes());
}

TEST(N2NetConnectionTest, AWindowOfOneStillDeliversUnderLoss)
{
    ConnectionConfig config = TestConfig();
    config.reliableWindow = 1;
    SimLinkConfig link;
    link.latencyMicros = 5000;
    link.lossPpm = 200'000;
    TwoPeers peers(4, link, config, config);
    SendAll(*peers.peerA.connection, 40);
    ASSERT_TRUE(peers.RunUntil([&] { return peers.peerB.received.size() >= 40; }, 600'000'000));
    ExpectInOrderExactlyOnce(peers.peerB.received, 40);
}

TEST(N2NetConnectionTest, AReceivedMessageOverTheCapEndsTheConnection)
{
    ConnectionConfig config = TestConfig();
    config.maxMessageSize = 2000;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> chunk(kMaxUnitPayload, 5);
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(0).Unit(0, kUnitFlagFirst, chunk).Build(), received), PacketStatus::Ok);
    EXPECT_FALSE(receiver.IsFailed());
    EXPECT_EQ(Feed(receiver, 200, PacketBuilder(1).Unit(1, 0, chunk).Build(), received), PacketStatus::Ok);
    EXPECT_TRUE(receiver.IsFailed()); // 2 x 1177 > 2000
    EXPECT_EQ(receiver.Failure(), FailureReason::MessageTooLarge);
    EXPECT_TRUE(received.empty());
    EXPECT_EQ(receiver.MemoryInUseBytes(), 0u);

    // a failed connection takes nothing more and sends nothing
    EXPECT_EQ(Feed(receiver, 300, PacketBuilder(2).Unreliable(0, chunk).Build(), received), PacketStatus::ConnectionFailed);
    EXPECT_EQ(receiver.SendReliable(chunk), SendResult::ConnectionFailed);
    EXPECT_EQ(receiver.SendUnreliable(chunk), SendResult::ConnectionFailed);
    EXPECT_TRUE(Capture(receiver, 10'000'000).empty());

    // a single unit over the cap is refused too
    ConnectionConfig tiny = TestConfig();
    tiny.maxMessageSize = 100;
    Connection other(tiny, 0);
    Feed(other, 100, PacketBuilder(0).Unit(0, kUnitFlagFirst | kUnitFlagLast, chunk).Build(), received);
    EXPECT_EQ(other.Failure(), FailureReason::MessageTooLarge);
}

TEST(N2NetConnectionTest, ImpossibleUnitSequencesEndTheConnection)
{
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data{1, 2, 3};

    Connection continuation(TestConfig(), 0); // a continuation with no start
    Feed(continuation, 100, PacketBuilder(0).Unit(0, 0, data).Build(), received);
    EXPECT_EQ(continuation.Failure(), FailureReason::ProtocolViolation);

    Connection doubleStart(TestConfig(), 0); // a start inside a message
    Feed(doubleStart, 100, PacketBuilder(0).Unit(0, kUnitFlagFirst, data).Unit(1, kUnitFlagFirst, data).Build(), received);
    EXPECT_EQ(doubleStart.Failure(), FailureReason::ProtocolViolation);

    Connection ok(TestConfig(), 0); // start, middle, end is fine
    Feed(ok, 100, PacketBuilder(0).Unit(0, kUnitFlagFirst, data).Unit(1, 0, data).Unit(2, kUnitFlagLast, data).Build(), received);
    EXPECT_FALSE(ok.IsFailed());
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].data.size(), 9u);
}

TEST(N2NetConnectionTest, UnitsOutsideTheReceiveWindowAreDroppedNotBuffered)
{
    ConnectionConfig config = TestConfig();
    config.reliableWindow = 16;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data(100, 1);
    // ids 0..15 are the window; 16 and 30000 are not (an id 32768 or more "ahead" counts as already delivered)
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(0).Unit(16, kUnitFlagFirst | kUnitFlagLast, data).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(1).Unit(30000, kUnitFlagFirst | kUnitFlagLast, data).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(11).Unit(40000, kUnitFlagFirst | kUnitFlagLast, data).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(receiver.Stats().unitsDuplicate, 1u);
    EXPECT_EQ(receiver.Stats().unitsOutOfWindow, 2u);
    EXPECT_EQ(receiver.MemoryInUseBytes(), 0u);
    // 15 is the last slot of the window: buffered, waiting for 0..14
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(2).Unit(15, kUnitFlagFirst | kUnitFlagLast, data).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(receiver.Stats().unitsOutOfWindow, 2u);
    EXPECT_GT(receiver.MemoryInUseBytes(), 0u);
    EXPECT_TRUE(received.empty());
    // a unit already delivered is a duplicate, not buffered
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(3).Unit(0, kUnitFlagFirst | kUnitFlagLast, data).Build(), received), PacketStatus::Ok);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(Feed(receiver, 100, PacketBuilder(4).Unit(0, kUnitFlagFirst | kUnitFlagLast, data).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(received.size(), 1u);
    EXPECT_EQ(receiver.Stats().unitsDuplicate, 2u);
}

TEST(N2NetConnectionTest, OnPacketNamesWhyADatagramWasRefused)
{
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data{1, 2, 3};

    EXPECT_EQ(Feed(receiver, 0, std::vector<std::uint8_t>(kMaxPacketSize + 1, 0), received), PacketStatus::TooLarge);
    EXPECT_EQ(Feed(receiver, 0, std::span<const std::uint8_t>(), received), PacketStatus::BadHeader);
    EXPECT_EQ(Feed(receiver, 0, std::vector<std::uint8_t>(kHeaderSize - 1, 0), received), PacketStatus::BadHeader);
    std::vector<std::uint8_t> reserved = PacketBuilder(0).Build();
    reserved[16] = 0x80;
    EXPECT_EQ(Feed(receiver, 0, reserved, received), PacketStatus::BadHeader);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(0).Protocol(1).Build(), received), PacketStatus::WrongProtocol);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(0).ConnectionId(1).Build(), received), PacketStatus::WrongConnection);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(0).Unit(0, kUnitFlagFirst | kUnitFlagLast, data).Raw(kBlockReliableUnit, 1, 3, 50, data).Build(), received),
              PacketStatus::Malformed);
    EXPECT_TRUE(received.empty()); // the valid block before the bad one was not applied
    EXPECT_EQ(receiver.Stats().packetsMalformed, 1u);
    EXPECT_EQ(receiver.Stats().packetsReceived, 0u);
    EXPECT_EQ(receiver.Stats().packetsRejected, 7u);

    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(5).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(5).Build(), received), PacketStatus::DuplicatePacket);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(200).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(6).Build(), received), PacketStatus::Ok); // 194 behind: still in the ring
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(6).Build(), received), PacketStatus::DuplicatePacket);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(4).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(300).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(40).Build(), received), PacketStatus::TooOld); // 260 behind
}

TEST(N2NetConnectionTest, AHugeForwardJumpOfThePacketNumberDoesNotConfuseDuplicateDetection)
{
    Connection receiver(TestConfig(), 0);
    std::vector<ReceivedMessage> received;
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(10).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(20000).Build(), received), PacketStatus::Ok);
    // 10 is far behind now: too old rather than a stale "seen" mark of the earlier packet
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(10).Build(), received), PacketStatus::TooOld);
    // coming back around: 10 + 65536 is the same number as 10, and 20000 + 45546 would be; a fresh number is fresh
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(20001).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(20001 + 255).Build(), received), PacketStatus::Ok);
    EXPECT_EQ(Feed(receiver, 0, PacketBuilder(20001 + 255 - 255 + 1).Build(), received), PacketStatus::Ok);
}

// ---- timeouts and keep-alive ----

TEST(N2NetConnectionTest, AnIncompleteMessageThatStopsMakingProgressEndsTheConnection)
{
    ConnectionConfig config = TestConfig();
    config.reassemblyTimeoutMicros = 5'000'000;
    config.idleTimeoutMicros = 1'000'000'000;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data(500, 2);
    Feed(receiver, 1'000'000, PacketBuilder(0).Unit(0, kUnitFlagFirst, data).Build(), received);
    Capture(receiver, 5'900'000);
    EXPECT_FALSE(receiver.IsFailed()); // 4.9 s since the start
    // progress (another unit of the message) restarts the clock
    Feed(receiver, 5'900'000, PacketBuilder(1).Unit(1, 0, data).Build(), received);
    Capture(receiver, 10'800'000);
    EXPECT_FALSE(receiver.IsFailed()); // 4.9 s since the progress
    Capture(receiver, 10'900'001);
    EXPECT_TRUE(receiver.IsFailed());
    EXPECT_EQ(receiver.Failure(), FailureReason::ReassemblyStall);
    EXPECT_EQ(receiver.MemoryInUseBytes(), 0u);
}

TEST(N2NetConnectionTest, UnitsWaitingBehindAGapAreTimedOutToo)
{
    ConnectionConfig config = TestConfig();
    config.reassemblyTimeoutMicros = 2'000'000;
    config.idleTimeoutMicros = 1'000'000'000;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data(10, 2);
    Feed(receiver, 0, PacketBuilder(0).Unit(3, kUnitFlagFirst | kUnitFlagLast, data).Build(), received); // 0..2 never come
    Capture(receiver, 1'999'999);
    EXPECT_FALSE(receiver.IsFailed());
    Capture(receiver, 2'000'001);
    EXPECT_EQ(receiver.Failure(), FailureReason::ReassemblyStall);
}

TEST(N2NetConnectionTest, ACompletedMessageClearsTheReassemblyClock)
{
    ConnectionConfig config = TestConfig();
    config.reassemblyTimeoutMicros = 2'000'000;
    config.idleTimeoutMicros = 1'000'000'000;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data(10, 2);
    Feed(receiver, 0, PacketBuilder(0).Unit(0, kUnitFlagFirst, data).Build(), received);
    Feed(receiver, 100, PacketBuilder(1).Unit(1, kUnitFlagLast, data).Build(), received);
    ASSERT_EQ(received.size(), 1u);
    Capture(receiver, 60'000'000);
    EXPECT_FALSE(receiver.IsFailed());
}

TEST(N2NetConnectionTest, SilenceEndsTheConnection)
{
    ConnectionConfig config = TestConfig();
    config.idleTimeoutMicros = 10'000'000;
    Connection connection(config, 500'000);
    Capture(connection, 10'400'000);
    EXPECT_FALSE(connection.IsFailed());
    Capture(connection, 10'500'001);
    EXPECT_EQ(connection.Failure(), FailureReason::Timeout);

    // anything received restarts the clock
    Connection alive(config, 0);
    std::vector<ReceivedMessage> received;
    Feed(alive, 9'000'000, PacketBuilder(0).Build(), received);
    Capture(alive, 18'000'000);
    EXPECT_FALSE(alive.IsFailed());
    Capture(alive, 19'000'001);
    EXPECT_EQ(alive.Failure(), FailureReason::Timeout);
}

TEST(N2NetConnectionTest, KeepAliveAndAckOnlyPackets)
{
    Connection connection(TestConfig(), 0);
    EXPECT_EQ(Capture(connection, 0).size(), 0u);
    EXPECT_EQ(Capture(connection, 999'999).size(), 0u);
    const Packets keepAlive = Capture(connection, 1'000'000);
    ASSERT_EQ(keepAlive.size(), 1u);
    EXPECT_EQ(keepAlive[0].size(), kHeaderSize);
    EXPECT_EQ(Capture(connection, 1'500'000).size(), 0u);
    EXPECT_EQ(Capture(connection, 2'000'000).size(), 1u);

    // a packet with blocks is acknowledged by one ack-only packet, at the next Update, and once
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data{1};
    Feed(connection, 2'100'000, PacketBuilder(0).Unreliable(0, data).Build(), received);
    const Packets ack = Capture(connection, 2'100'001);
    ASSERT_EQ(ack.size(), 1u);
    PacketHeader header;
    ByteReader reader(ack[0]);
    ASSERT_TRUE(ReadHeader(reader, header));
    EXPECT_TRUE(header.hasAck);
    EXPECT_EQ(header.ack, 0);
    EXPECT_EQ(Capture(connection, 2'100'002).size(), 0u);

    // a packet with no blocks (an ack or keep-alive) is not acknowledged: no ping-pong
    Feed(connection, 2'200'000, PacketBuilder(1).Build(), received);
    EXPECT_EQ(Capture(connection, 2'200'001).size(), 0u);
    // a repeat of a packet with blocks is acknowledged again (the first ack may have been lost)
    Feed(connection, 2'300'000, PacketBuilder(0).Unreliable(0, data).Build(), received);
    EXPECT_EQ(Capture(connection, 2'300'001).size(), 1u);
}

TEST(N2NetConnectionTest, TheFirstPacketsCarryNoAck)
{
    Connection connection(TestConfig(), 0);
    ASSERT_EQ(connection.SendUnreliable(std::vector<std::uint8_t>{1}), SendResult::Ok);
    const Packets out = Capture(connection, 1000);
    ASSERT_EQ(out.size(), 1u);
    PacketHeader header;
    ByteReader reader(out[0]);
    ASSERT_TRUE(ReadHeader(reader, header));
    EXPECT_FALSE(header.hasAck); // nothing received yet: no ack 0 that could be taken for packet 0
    EXPECT_EQ(header.protocolId, kTestProtocolId);
    EXPECT_EQ(header.connectionId, kTestConnectionId);
    EXPECT_EQ(header.sequence, 0);
}

TEST(N2NetConnectionTest, MaxPacketsPerUpdateBoundsTheWorkOfOneCall)
{
    ConnectionConfig config = TestConfig();
    config.maxPacketsPerUpdate = 5;
    Connection connection(config, 0);
    ASSERT_EQ(connection.SendReliable(MakeMessage(1, 60'000)), SendResult::Ok);
    EXPECT_EQ(Capture(connection, 1000).size(), 5u);
    EXPECT_EQ(Capture(connection, 1000).size(), 5u);
}

// ---- statistics ----

TEST(N2NetConnectionTest, RttConvergesOnTheLinkRoundTrip)
{
    SimLinkConfig link;
    link.latencyMicros = 25'000;
    TwoPeers peers(6, link);
    for (std::uint32_t i = 0; i < 40; ++i)
    {
        ASSERT_EQ(peers.peerA.connection->SendReliable(MakeMessage(i, 50)), SendResult::Ok);
        peers.RunFor(100'000);
    }
    const RttEstimator &rtt = peers.peerA.connection->Rtt();
    ASSERT_TRUE(rtt.HasSample());
    EXPECT_GE(rtt.SmoothedMicros(), 50'000u);
    EXPECT_LE(rtt.SmoothedMicros(), 52'000u);
    EXPECT_GE(rtt.MinMicros(), 50'000u);
    EXPECT_LE(rtt.VarianceMicros(), 2000u);
    EXPECT_GE(rtt.RtoMicros(), peers.peerA.connection->Config().minRtoMicros);
    EXPECT_LE(rtt.RtoMicros(), 70'000u); // srtt + 4 * variance: a steady link has a tight RTO
    EXPECT_EQ(peers.peerA.connection->LossRatePpm(), 0u);
    // B only ever acknowledged; it has no data in flight, so no RTT of its own to measure
    EXPECT_FALSE(peers.peerB.connection->Rtt().HasSample());
}

TEST(N2NetConnectionTest, LossStatisticsTrackTheLink)
{
    SimLinkConfig link;
    link.latencyMicros = 20'000;
    link.lossPpm = 300'000;
    TwoPeers peers(7, link);
    for (std::uint32_t i = 0; i < 1500; ++i)
    {
        ASSERT_EQ(peers.peerA.connection->SendReliable(MakeMessage(i, 40)), SendResult::Ok);
        peers.RunFor(5000);
    }
    peers.RunFor(10'000'000);
    const std::uint32_t loss = peers.peerA.connection->LossRatePpm();
    EXPECT_GT(loss, 200'000u);
    EXPECT_LT(loss, 450'000u);
    EXPECT_TRUE(peers.peerA.connection->Rtt().HasSample());
    EXPECT_GE(peers.peerA.connection->Rtt().SmoothedMicros(), 40'000u);
}

// ---- stream transports ----

TEST(N2NetConnectionTest, StreamModeSendsOnceAndKeepsNothingForResends)
{
    ConnectionConfig config = TestConfig();
    config.reliableStream = true;
    SimLinkConfig link;
    link.latencyMicros = 10'000;
    TwoPeers peers(8, link, config, config);
    SendAll(*peers.peerA.connection, 200);
    ASSERT_TRUE(peers.RunUntil([&] { return peers.peerB.received.size() >= 200; }, 60'000'000));
    ExpectInOrderExactlyOnce(peers.peerB.received, 200);
    peers.RunFor(200'000); // the acks come back
    EXPECT_EQ(peers.peerA.connection->Stats().unitsResent, 0u);
    EXPECT_EQ(peers.peerA.connection->Stats().packetsLost, 0u);
    EXPECT_TRUE(peers.peerA.connection->ReliableIdle());
    EXPECT_EQ(peers.peerA.connection->MemoryInUseBytes(), 0u); // nothing retained after sending
    // the sequence numbers and acks still work: there are RTT samples
    EXPECT_TRUE(peers.peerA.connection->Rtt().HasSample());
}

TEST(N2NetConnectionTest, StreamModeRefusesUnitsOutOfOrder)
{
    ConnectionConfig config = TestConfig();
    config.reliableStream = true;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> data{1};
    Feed(receiver, 0, PacketBuilder(0).Unit(0, kUnitFlagFirst | kUnitFlagLast, data).Build(), received);
    EXPECT_EQ(received.size(), 1u);
    EXPECT_FALSE(receiver.IsFailed());
    Feed(receiver, 0, PacketBuilder(1).Unit(2, kUnitFlagFirst | kUnitFlagLast, data).Build(), received); // 1 skipped
    EXPECT_EQ(receiver.Failure(), FailureReason::ProtocolViolation);
    EXPECT_EQ(received.size(), 1u);
}

// ---- memory ----

TEST(N2NetConnectionTest, MemoryStaysUnderTheBoundWithAFullWindowOfMaxSizeUnits)
{
    ConnectionConfig config = TestConfig();
    config.reliableWindow = 64;
    Connection receiver(config, 0);
    std::vector<ReceivedMessage> received;
    const std::vector<std::uint8_t> full(kMaxUnitPayload, 9);
    // every slot but the first, each a full unit: the worst the window allows
    std::uint16_t packet = 0;
    for (std::uint16_t id = 1; id < 64; ++id)
    {
        Feed(receiver, 0, PacketBuilder(packet++).Unit(id, kUnitFlagFirst | kUnitFlagLast, full).Build(), received);
    }
    EXPECT_GT(receiver.MemoryInUseBytes(), 60 * kMaxUnitPayload);
    EXPECT_LE(receiver.MemoryInUseBytes(), receiver.MemoryBoundBytes());
    EXPECT_TRUE(received.empty());
}
