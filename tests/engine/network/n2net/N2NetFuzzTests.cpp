#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Packet.hpp"
#include "engine/network/n2net/SimRandom.hpp"

// Hostile input (net-a3): this layer will face the internet, so thousands of random, truncated, mutated and structurally
// plausible datagrams are fed to a connection. Nothing may crash, read out of bounds (run under ASan/UBSan locally),
// loop, deliver a message over the caps, or hold more memory than MemoryBoundBytes. Seeded, so a failure reproduces; the
// iteration counts are small enough for the web (wasm under node) leg.

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

namespace
{
    ConnectionConfig FuzzConfig()
    {
        ConnectionConfig config = TestConfig();
        config.maxMessageSize = 4000;
        config.maxSendQueueBytes = 64 * 1024;
        config.reliableWindow = 16;
        config.maxQueuedUnreliableMessages = 8;
        config.maxQueuedUnreliableBytes = 8 * 1024;
        return config;
    }

    /// A pool of real packets from a sender with reliable, fragmented and unreliable traffic
    std::vector<std::vector<std::uint8_t>> ValidPool()
    {
        std::vector<std::vector<std::uint8_t>> pool;
        Connection sender(TestConfig(), 0);
        for (std::uint32_t round = 0; round < 12; ++round)
        {
            sender.SendReliable(MakeMessage(round, 1 + (round * 523u) % 3500u));
            sender.SendReliable(MakeMessage(round + 100, 30));
            sender.SendUnreliable(MakeMessage(round + 200, 64));
            sender.Update(1000 + round, [&](std::span<const std::uint8_t> packet) { pool.emplace_back(packet.begin(), packet.end()); });
        }
        return pool;
    }

    struct Checker
    {
        const ConnectionConfig &config;
        std::uint64_t delivered = 0;

        void operator()(MessageChannel channel, std::span<const std::uint8_t> message)
        {
            ++delivered;
            EXPECT_FALSE(message.empty());
            if (channel == MessageChannel::ReliableOrdered)
            {
                EXPECT_LE(message.size(), config.maxMessageSize);
            }
            else
            {
                EXPECT_LE(message.size(), kMaxUnreliablePayload);
            }
        }
    };

    void Feed(Connection &connection, Checker &checker, std::uint64_t now, std::span<const std::uint8_t> packet)
    {
        connection.OnPacket(now, packet, [&](MessageChannel channel, std::span<const std::uint8_t> message) { checker(channel, message); });
        ASSERT_LE(connection.MemoryInUseBytes(), connection.MemoryBoundBytes());
    }

    std::vector<std::uint8_t> RandomBytes(SimRandom &random, std::size_t length)
    {
        std::vector<std::uint8_t> bytes(length);
        for (std::uint8_t &byte : bytes)
        {
            byte = static_cast<std::uint8_t>(random.NextBelow(256));
        }
        return bytes;
    }

    /// The right ids and header, then `length` random bytes
    std::vector<std::uint8_t> HeaderThenRandom(SimRandom &random, std::size_t length)
    {
        std::vector<std::uint8_t> packet = PacketBuilder(static_cast<std::uint16_t>(random.NextBelow(65536)))
                                               .Ack(static_cast<std::uint16_t>(random.NextBelow(65536)), static_cast<std::uint32_t>(random.Next()))
                                               .Build();
        const std::vector<std::uint8_t> tail = RandomBytes(random, length);
        packet.insert(packet.end(), tail.begin(), tail.end());
        return packet;
    }

    /// Right ids, then blocks that are mostly well formed: random types, ids, flags and lengths (some lying)
    std::vector<std::uint8_t> PlausibleBlocks(SimRandom &random, std::uint16_t baseUnit)
    {
        PacketBuilder builder(static_cast<std::uint16_t>(random.NextBelow(65536)));
        if (random.NextBelow(2) == 0)
        {
            builder.Ack(static_cast<std::uint16_t>(random.NextBelow(65536)), static_cast<std::uint32_t>(random.Next()));
        }
        const std::uint32_t blocks = random.NextBelow(6);
        for (std::uint32_t i = 0; i < blocks; ++i)
        {
            const std::size_t length = random.NextBelow(5) == 0 ? random.NextBelow(1300) : 1 + random.NextBelow(200);
            const std::vector<std::uint8_t> data = RandomBytes(random, std::min<std::size_t>(length, 400));
            // near the window most of the time, anywhere sometimes
            const std::uint16_t id = random.NextBelow(4) == 0 ? static_cast<std::uint16_t>(random.NextBelow(65536))
                                                              : static_cast<std::uint16_t>(baseUnit + random.NextBelow(24));
            const std::uint8_t flags = static_cast<std::uint8_t>(random.NextBelow(4));
            if (random.NextBelow(3) == 0)
            {
                builder.Unreliable(static_cast<std::uint16_t>(random.NextBelow(65536)), data);
            }
            else if (random.NextBelow(8) == 0)
            {
                builder.Raw(static_cast<std::uint8_t>(random.NextBelow(5)), id, flags, static_cast<std::uint16_t>(length), data);
            }
            else
            {
                builder.Unit(id, flags, data);
            }
        }
        return builder.Build();
    }

    std::vector<std::uint8_t> Mutate(SimRandom &random, std::vector<std::uint8_t> packet)
    {
        const std::uint32_t kind = random.NextBelow(5);
        if (kind == 0 && !packet.empty())
        {
            packet.resize(random.NextBelow(static_cast<std::uint32_t>(packet.size()) + 1)); // truncate
        }
        else if (kind == 1 && !packet.empty())
        {
            for (std::uint32_t i = 0, flips = 1 + random.NextBelow(8); i < flips; ++i)
            {
                packet[random.NextBelow(static_cast<std::uint32_t>(packet.size()))] ^= static_cast<std::uint8_t>(1 + random.NextBelow(255));
            }
        }
        else if (kind == 2)
        {
            const std::vector<std::uint8_t> extra = RandomBytes(random, 1 + random.NextBelow(100));
            packet.insert(packet.end(), extra.begin(), extra.end()); // extend
        }
        else if (kind == 3 && packet.size() > 2)
        {
            const std::size_t at = random.NextBelow(static_cast<std::uint32_t>(packet.size()));
            packet.erase(packet.begin() + static_cast<std::ptrdiff_t>(at)); // delete one byte
        }
        else if (!packet.empty())
        {
            packet[random.NextBelow(static_cast<std::uint32_t>(packet.size()))] = 0xFF;
        }
        if (packet.size() > kMaxPacketSize + 50)
        {
            packet.resize(kMaxPacketSize + 50);
        }
        return packet;
    }
} // namespace

TEST(N2NetFuzzTest, RandomTruncatedAndMutatedPacketsNeverBreakAConnection)
{
    const ConnectionConfig config = FuzzConfig();
    const std::vector<std::vector<std::uint8_t>> pool = ValidPool();
    ASSERT_GT(pool.size(), 8u);
    SimRandom random(0xF00D);
    auto connection = std::make_unique<Connection>(config, 0);
    Checker checker{config};
    std::uint64_t now = 1000;
    std::uint64_t failures = 0;
    std::uint64_t accepted = 0;
    constexpr int kIterations = 40000;
    for (int i = 0; i < kIterations; ++i)
    {
        now += 1 + random.NextBelow(5000);
        std::vector<std::uint8_t> packet;
        switch (random.NextBelow(6))
        {
        case 0:
            packet = RandomBytes(random, random.NextBelow(1300));
            break;
        case 1:
            packet = HeaderThenRandom(random, random.NextBelow(1200 - static_cast<std::uint32_t>(kHeaderSize)));
            break;
        case 2:
            packet = Mutate(random, pool[random.NextBelow(static_cast<std::uint32_t>(pool.size()))]);
            break;
        case 3:
            packet = PlausibleBlocks(random, static_cast<std::uint16_t>(i / 7));
            break;
        case 4:
            packet = pool[random.NextBelow(static_cast<std::uint32_t>(pool.size()))]; // a valid one, maybe repeated
            break;
        default:
            packet = Mutate(random, PlausibleBlocks(random, static_cast<std::uint16_t>(i / 7)));
            break;
        }

        const std::uint64_t before = connection->Stats().packetsReceived;
        Feed(*connection, checker, now, packet);
        if (HasFatalFailure())
        {
            return;
        }
        accepted += connection->Stats().packetsReceived - before;

        if (i % 50 == 0)
        {
            // Whatever the peer did to it, what the connection sends is always a valid datagram
            connection->SendReliable(MakeMessage(static_cast<std::uint32_t>(i), 1 + random.NextBelow(3000)));
            connection->SendUnreliable(MakeMessage(static_cast<std::uint32_t>(i), 1 + random.NextBelow(500)));
            connection->Update(now,
                               [&](std::span<const std::uint8_t> out)
                               {
                                   EXPECT_LE(out.size(), kMaxPacketSize);
                                   EXPECT_GE(out.size(), kHeaderSize);
                                   std::size_t blocks = 0;
                                   EXPECT_TRUE(ValidateBlocks(out.subspan(kHeaderSize), blocks));
                               });
            ASSERT_LE(connection->MemoryInUseBytes(), connection->MemoryBoundBytes());
        }
        if (connection->IsFailed())
        {
            ++failures;
            connection = std::make_unique<Connection>(config, now);
        }
    }
    // The run did reach the parser and the channels: a fair share was accepted, some ended the connection, and
    // messages were delivered
    EXPECT_GT(accepted, 1000u);
    EXPECT_GT(failures, 0u);
    EXPECT_GT(checker.delivered, 100u);
}

TEST(N2NetFuzzTest, EveryPrefixOfAValidPacketIsHandled)
{
    const ConnectionConfig config = TestConfig();
    const std::vector<std::vector<std::uint8_t>> pool = ValidPool();
    for (std::size_t p = 0; p < pool.size(); p += 3)
    {
        const std::vector<std::uint8_t> &packet = pool[p];
        for (std::size_t length = 0; length <= packet.size(); ++length)
        {
            Connection connection(config, 0);
            Checker checker{config};
            const std::span<const std::uint8_t> prefix(packet.data(), length);
            const PacketStatus status = connection.OnPacket(
                10, prefix, [&](MessageChannel channel, std::span<const std::uint8_t> message) { checker(channel, message); });

            std::size_t blocks = 0;
            const bool parses = length >= kHeaderSize && ValidateBlocks(prefix.subspan(kHeaderSize), blocks);
            if (parses)
            {
                EXPECT_EQ(status, PacketStatus::Ok) << p << "/" << length;
            }
            else
            {
                EXPECT_NE(status, PacketStatus::Ok) << p << "/" << length;
                EXPECT_EQ(checker.delivered, 0u) << p << "/" << length; // a refused packet delivers nothing
            }
        }
    }
}

TEST(N2NetFuzzTest, EverySingleByteChangeOfAValidPacketIsHandled)
{
    const ConnectionConfig config = TestConfig();
    const std::vector<std::vector<std::uint8_t>> pool = ValidPool();
    for (std::size_t p = 0; p < pool.size(); p += 4)
    {
        for (std::size_t at = 0; at < pool[p].size(); ++at)
        {
            for (std::uint8_t value : {std::uint8_t{0x00}, std::uint8_t{0xFF}, std::uint8_t{0x80}, std::uint8_t{0x7F}})
            {
                std::vector<std::uint8_t> packet = pool[p];
                packet[at] = value;
                Connection connection(config, 0);
                Checker checker{config};
                connection.OnPacket(10, packet, [&](MessageChannel channel, std::span<const std::uint8_t> message) { checker(channel, message); });
                ASSERT_LE(connection.MemoryInUseBytes(), connection.MemoryBoundBytes());
            }
        }
    }
}

TEST(N2NetFuzzTest, AFloodOfMaxSizeUnitsAtEveryIdStaysInsideTheWindowBudget)
{
    ConnectionConfig config = TestConfig();
    config.reliableWindow = 32;
    Connection connection(config, 0);
    Checker checker{config};
    const std::vector<std::uint8_t> full(kMaxUnitPayload, 0xAB);
    SimRandom random(99);
    for (std::uint32_t i = 0; i < 20000; ++i)
    {
        // first units never completed (flags first only) at random ids, so nothing is ever delivered or freed fast
        const std::vector<std::uint8_t> packet = PacketBuilder(static_cast<std::uint16_t>(i))
                                                     .Unit(static_cast<std::uint16_t>(1 + random.NextBelow(60)), kUnitFlagFirst, full)
                                                     .Build();
        Feed(connection, checker, 100 + i, packet);
        if (HasFatalFailure())
        {
            return;
        }
        if (connection.IsFailed())
        {
            break;
        }
    }
    // unit 0 never came, so nothing was delivered; what was buffered is bounded by the window
    EXPECT_EQ(checker.delivered, 0u);
    EXPECT_LE(connection.MemoryInUseBytes(), 32u * kMaxUnitPayload);
}

TEST(N2NetFuzzTest, AnEndlessMessageEndsAtTheCapNotWhenMemoryRunsOut)
{
    ConnectionConfig config = TestConfig();
    config.maxMessageSize = 50'000;
    Connection connection(config, 0);
    Checker checker{config};
    const std::vector<std::uint8_t> full(kMaxUnitPayload, 0xCD);
    std::uint16_t id = 0;
    std::size_t units = 0;
    // a first unit and then continuations that never say "last"
    for (; units < 1000 && !connection.IsFailed(); ++units, ++id)
    {
        const std::uint8_t flags = id == 0 ? kUnitFlagFirst : 0;
        Feed(connection, checker, 100, PacketBuilder(id).Unit(id, flags, full).Build());
        if (HasFatalFailure())
        {
            return;
        }
        EXPECT_LE(connection.MemoryInUseBytes(), connection.MemoryBoundBytes());
    }
    EXPECT_TRUE(connection.IsFailed());
    EXPECT_EQ(connection.Failure(), FailureReason::MessageTooLarge);
    EXPECT_LE(units * kMaxUnitPayload, 50'000u + 2 * kMaxUnitPayload); // it stopped at the cap
    EXPECT_EQ(checker.delivered, 0u);
}

TEST(N2NetFuzzTest, AFullPacketOfTinyBlocksIsParsedInBoundedTime)
{
    // 1-byte unreliable messages are the densest blocks: 6 bytes each, about 196 in one datagram
    PacketBuilder builder(0);
    const std::array<std::uint8_t, 1> one{1};
    std::size_t blocks = 0;
    while (kHeaderSize + (blocks + 1) * 6 <= kMaxPacketSize)
    {
        builder.Unreliable(static_cast<std::uint16_t>(blocks), one);
        ++blocks;
    }
    const std::vector<std::uint8_t> packet = builder.Build();
    ASSERT_LE(packet.size(), kMaxPacketSize);
    ASSERT_GT(blocks, 150u);
    Connection connection(TestConfig(), 0);
    std::uint64_t delivered = 0;
    EXPECT_EQ(connection.OnPacket(1, packet, [&](MessageChannel, std::span<const std::uint8_t>) { ++delivered; }), PacketStatus::Ok);
    EXPECT_EQ(delivered, blocks);
}
