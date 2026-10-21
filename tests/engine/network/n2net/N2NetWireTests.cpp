#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "N2NetTestUtil.hpp"
#include "engine/network/n2net/ByteIO.hpp"
#include "engine/network/n2net/Packet.hpp"
#include "engine/network/n2net/RttEstimator.hpp"
#include "engine/network/n2net/Sequence.hpp"
#include "engine/network/n2net/SimRandom.hpp"

// n2net wire format and small parts (net-a3): sequence comparison across the 16-bit wrap, the bounds-checked byte
// reader and writer, the packet header and block layout byte for byte, the simulator's PRNG and the RTT estimator.

using namespace N2Engine::N2Net;
using namespace N2Engine::N2Net::Testing;

TEST(N2NetSequenceTest, MoreRecentAcrossTheWrap)
{
    EXPECT_TRUE(SequenceMoreRecent(1, 0));
    EXPECT_FALSE(SequenceMoreRecent(0, 1));
    EXPECT_FALSE(SequenceMoreRecent(5, 5));
    // 0 is 1 step after 65535, and 65535 is 1 step before 0
    EXPECT_TRUE(SequenceMoreRecent(0, 65535));
    EXPECT_FALSE(SequenceMoreRecent(65535, 0));
    EXPECT_TRUE(SequenceMoreRecent(10, 65530));
    EXPECT_FALSE(SequenceMoreRecent(65530, 10));
    // the half-way point is ambiguous and neither is more recent; one step either side is decided
    EXPECT_FALSE(SequenceMoreRecent(32768, 0));
    EXPECT_FALSE(SequenceMoreRecent(0, 32768));
    EXPECT_TRUE(SequenceMoreRecent(32767, 0));
    EXPECT_FALSE(SequenceMoreRecent(0, 32767));
    EXPECT_TRUE(SequenceMoreRecent(0, 32769));
}

TEST(N2NetSequenceTest, DistanceWraps)
{
    EXPECT_EQ(SequenceDistance(65535, 0), 1);
    EXPECT_EQ(SequenceDistance(0, 65535), 65535);
    EXPECT_EQ(SequenceDistance(7, 7), 0);
    EXPECT_EQ(SequenceDistance(65530, 4), 10);
}

TEST(N2NetByteIoTest, WriterIsLittleEndianAndNeverOverflows)
{
    std::array<std::uint8_t, 8> buffer{};
    ByteWriter writer(buffer.data(), 7); // one byte of the array is out of bounds for the writer
    writer.U8(0x01);
    writer.U16(0x0302);
    writer.U32(0x07060504);
    EXPECT_FALSE(writer.Overflowed());
    EXPECT_EQ(writer.Size(), 7u);
    const std::array<std::uint8_t, 8> expected{1, 2, 3, 4, 5, 6, 7, 0};
    EXPECT_EQ(buffer, expected);

    writer.U8(0xFF); // no room: nothing written
    EXPECT_TRUE(writer.Overflowed());
    EXPECT_EQ(writer.Size(), 7u);
    EXPECT_EQ(buffer[7], 0);
}

TEST(N2NetByteIoTest, ReaderChecksEveryRead)
{
    const std::array<std::uint8_t, 7> bytes{1, 2, 3, 4, 5, 6, 7};
    ByteReader reader(bytes);
    EXPECT_EQ(reader.U8(), 1);
    EXPECT_EQ(reader.U16(), 0x0302);
    EXPECT_EQ(reader.U32(), 0x07060504u);
    EXPECT_FALSE(reader.Failed());
    EXPECT_EQ(reader.Remaining(), 0u);

    EXPECT_EQ(reader.U8(), 0);
    EXPECT_TRUE(reader.Failed());

    ByteReader shortReader(std::span<const std::uint8_t>(bytes.data(), 3));
    EXPECT_EQ(shortReader.U32(), 0u);
    EXPECT_TRUE(shortReader.Failed());

    ByteReader hugeRequest(bytes);
    EXPECT_TRUE(hugeRequest.Bytes(static_cast<std::size_t>(-1)).empty());
    EXPECT_TRUE(hugeRequest.Failed());
}

TEST(N2NetPacketTest, HeaderLayoutByteForByte)
{
    PacketHeader header;
    header.protocolId = 0x04030201;
    header.connectionId = 0x08070605;
    header.sequence = 0x0A09;
    header.ack = 0x0C0B;
    header.ackBits = 0x100F0E0D;
    header.hasAck = true;

    std::array<std::uint8_t, kHeaderSize> bytes{};
    ByteWriter writer(bytes.data(), bytes.size());
    ASSERT_TRUE(WriteHeader(writer, header));
    const std::array<std::uint8_t, kHeaderSize> expected{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                                         0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x01};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(kHeaderSize, 17u);

    PacketHeader parsed;
    ByteReader reader(bytes);
    ASSERT_TRUE(ReadHeader(reader, parsed));
    EXPECT_EQ(parsed.protocolId, header.protocolId);
    EXPECT_EQ(parsed.connectionId, header.connectionId);
    EXPECT_EQ(parsed.sequence, header.sequence);
    EXPECT_EQ(parsed.ack, header.ack);
    EXPECT_EQ(parsed.ackBits, header.ackBits);
    EXPECT_TRUE(parsed.hasAck);
}

TEST(N2NetPacketTest, HeaderWithoutAckWritesZeroAckFields)
{
    PacketHeader header;
    header.protocolId = 1;
    header.connectionId = 2;
    header.sequence = 3;
    header.ack = 0x1234;     // ignored: hasAck is false
    header.ackBits = 0xFFFF; // ignored
    std::array<std::uint8_t, kHeaderSize> bytes{};
    ByteWriter writer(bytes.data(), bytes.size());
    ASSERT_TRUE(WriteHeader(writer, header));
    for (std::size_t i = 10; i < kHeaderSize; ++i)
    {
        EXPECT_EQ(bytes[i], 0) << i;
    }
}

TEST(N2NetPacketTest, HeaderRejectsReservedFlagsAndTruncation)
{
    std::array<std::uint8_t, kHeaderSize> bytes{};
    bytes[16] = 0x02; // reserved bit
    PacketHeader header;
    ByteReader reserved(bytes);
    EXPECT_FALSE(ReadHeader(reserved, header));

    bytes[16] = 0x01;
    for (std::size_t length = 0; length < kHeaderSize; ++length)
    {
        ByteReader truncated(std::span<const std::uint8_t>(bytes.data(), length));
        EXPECT_FALSE(ReadHeader(truncated, header)) << length;
    }
}

TEST(N2NetPacketTest, BlockLayoutByteForByte)
{
    const std::array<std::uint8_t, 3> unitData{0xAA, 0xBB, 0xCC};
    const std::array<std::uint8_t, 2> unreliableData{0xDD, 0xEE};
    const std::vector<std::uint8_t> packet = PacketBuilder(1)
                                                 .Unit(0x0201, kUnitFlagFirst | kUnitFlagLast, unitData)
                                                 .Unreliable(0x0403, unreliableData)
                                                 .Build();
    const std::vector<std::uint8_t> blocks(packet.begin() + kHeaderSize, packet.end());
    const std::vector<std::uint8_t> expected{
        0x01, 0x01, 0x02, 0x03, 0x03, 0x00, 0xAA, 0xBB, 0xCC, // type 1, id, flags, length, data
        0x02, 0x03, 0x04, 0x02, 0x00, 0xDD, 0xEE,             // type 2, sequence, length, data
    };
    EXPECT_EQ(blocks, expected);

    ByteReader reader(blocks);
    PacketBlock block;
    ASSERT_EQ(ReadBlock(reader, block), BlockResult::Block);
    EXPECT_EQ(block.type, kBlockReliableUnit);
    EXPECT_EQ(block.id, 0x0201);
    EXPECT_EQ(block.flags, kUnitFlagFirst | kUnitFlagLast);
    EXPECT_EQ(std::vector<std::uint8_t>(block.data.begin(), block.data.end()), (std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC}));
    ASSERT_EQ(ReadBlock(reader, block), BlockResult::Block);
    EXPECT_EQ(block.type, kBlockUnreliable);
    EXPECT_EQ(block.id, 0x0403);
    EXPECT_EQ(block.data.size(), 2u);
    EXPECT_EQ(ReadBlock(reader, block), BlockResult::End);

    EXPECT_EQ(kUnitHeaderSize, 6u);
    EXPECT_EQ(kUnreliableHeaderSize, 5u);
    EXPECT_EQ(kMaxUnitPayload, 1177u);
    EXPECT_EQ(kMaxUnreliablePayload, 1178u);
    EXPECT_EQ(kMaxPacketSize, 1200u);
}

TEST(N2NetPacketTest, MalformedBlocksAreRefused)
{
    const std::array<std::uint8_t, 4> data{1, 2, 3, 4};
    const auto valid = [](const std::vector<std::uint8_t> &packet)
    {
        std::size_t count = 0;
        return ValidateBlocks(std::span<const std::uint8_t>(packet).subspan(kHeaderSize), count);
    };

    // a length past the end of the datagram
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(kBlockReliableUnit, 0, kUnitFlagFirst, 5, data).Build()));
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(kBlockReliableUnit, 0, kUnitFlagFirst, 0xFFFF, data).Build()));
    // a zero length
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(kBlockReliableUnit, 0, kUnitFlagFirst, 0, {}).Build()));
    // a length over the unit cap, even with the bytes present
    const std::vector<std::uint8_t> big(kMaxUnitPayload + 1, 0x11);
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(kBlockReliableUnit, 0, kUnitFlagFirst, static_cast<std::uint16_t>(big.size()), big).Build()));
    const std::vector<std::uint8_t> exact(kMaxUnitPayload, 0x11);
    EXPECT_TRUE(valid(PacketBuilder(1).Raw(kBlockReliableUnit, 0, kUnitFlagFirst, static_cast<std::uint16_t>(exact.size()), exact).Build()));
    // a reserved unit flag
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(kBlockReliableUnit, 0, 0x04, 4, data).Build()));
    // an unknown or zero block type
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(3, 0, 0, 4, data).Build()));
    EXPECT_FALSE(valid(PacketBuilder(1).Raw(0, 0, 0, 4, data).Build()));
    // bytes after the last block that are not a block
    EXPECT_FALSE(valid(PacketBuilder(1).Unit(0, kUnitFlagFirst | kUnitFlagLast, data).Bytes(std::vector<std::uint8_t>{0x01}).Build()));
    // a truncated block header
    EXPECT_FALSE(valid(PacketBuilder(1).Bytes(std::vector<std::uint8_t>{kBlockReliableUnit, 0x00}).Build()));
    // no blocks at all is valid, and so is a mix
    EXPECT_TRUE(valid(PacketBuilder(1).Build()));
    EXPECT_TRUE(valid(PacketBuilder(1).Unit(0, kUnitFlagFirst | kUnitFlagLast, data).Unreliable(0, data).Build()));
}

TEST(N2NetSimRandomTest, StreamIsFixedAcrossPlatforms)
{
    // Golden values: xoshiro256** seeded through SplitMix64 must give these on every compiler and architecture
    SimRandom random(0x1234'5678'9ABC'DEF0ull);
    std::uint64_t hash = kFnvOffset;
    for (int i = 0; i < 1000; ++i)
    {
        hash = HashU64(hash, random.Next());
    }
    EXPECT_EQ(hash, 0xD9715C4F9289F2B0ull);
}

TEST(N2NetSimRandomTest, NextBelowStaysInRangeAndIsSeedDependent)
{
    SimRandom random(7);
    for (int i = 0; i < 10000; ++i)
    {
        EXPECT_LT(random.NextBelow(10), 10u);
    }
    EXPECT_EQ(random.NextBelow(0), 0u);
    EXPECT_EQ(random.NextBelow(1), 0u);

    SimRandom a(1);
    SimRandom b(2);
    bool differs = false;
    for (int i = 0; i < 8; ++i)
    {
        differs = differs || a.Next() != b.Next();
    }
    EXPECT_TRUE(differs);

    // streams for different links are independent of each other
    SimRandom link12 = SimRandom::ForStream(5, 1, 2);
    SimRandom link21 = SimRandom::ForStream(5, 2, 1);
    SimRandom link12Again = SimRandom::ForStream(5, 1, 2);
    EXPECT_NE(link12.Next(), link21.Next());
    SimRandom copy = SimRandom::ForStream(5, 1, 2);
    EXPECT_EQ(link12Again.Next(), copy.Next());
}

TEST(N2NetRttTest, FirstSampleThenSmoothing)
{
    RttEstimator rtt(200'000, 10'000, 2'000'000);
    EXPECT_FALSE(rtt.HasSample());
    EXPECT_EQ(rtt.RtoMicros(), 200'000u); // the initial RTO before any sample

    rtt.AddSample(100'000);
    EXPECT_TRUE(rtt.HasSample());
    EXPECT_EQ(rtt.SmoothedMicros(), 100'000u);
    EXPECT_EQ(rtt.VarianceMicros(), 50'000u);
    EXPECT_EQ(rtt.RtoMicros(), 300'000u); // srtt + 4 * var

    rtt.AddSample(180'000);
    // error 80000: var = (3 * 50000 + 80000) / 4 = 57500; srtt = (7 * 100000 + 180000) / 8 = 110000
    EXPECT_EQ(rtt.VarianceMicros(), 57'500u);
    EXPECT_EQ(rtt.SmoothedMicros(), 110'000u);
    EXPECT_EQ(rtt.RtoMicros(), 340'000u);
    EXPECT_EQ(rtt.LatestMicros(), 180'000u);
    EXPECT_EQ(rtt.MinMicros(), 100'000u);
}

TEST(N2NetRttTest, ConvergesOnASteadyRttAndClampsTheRto)
{
    RttEstimator rtt(200'000, 50'000, 400'000);
    for (int i = 0; i < 200; ++i)
    {
        rtt.AddSample(60'000);
    }
    EXPECT_NEAR(static_cast<double>(rtt.SmoothedMicros()), 60'000.0, 100.0);
    EXPECT_LE(rtt.VarianceMicros(), 100u);
    // srtt + max(1 ms, 4 * var) is about 61 ms: inside the clamp
    EXPECT_NEAR(static_cast<double>(rtt.RtoMicros()), 61'000.0, 500.0);

    RttEstimator fast(200'000, 50'000, 400'000);
    fast.AddSample(1000);
    EXPECT_EQ(fast.RtoMicros(), 50'000u); // clamped up to the minimum

    RttEstimator slow(200'000, 50'000, 400'000);
    slow.AddSample(900'000);
    EXPECT_EQ(slow.RtoMicros(), 400'000u); // clamped down to the maximum
}
