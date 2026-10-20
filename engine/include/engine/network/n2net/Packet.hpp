#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/network/n2net/ByteIO.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::N2Net
{
    // The wire format (all integers little-endian). One datagram is a header followed by blocks, at most
    // kMaxPacketSize bytes in all.
    //
    //   header (17 bytes)
    //     0  u32  protocol id      what the receiver expects, or the packet is dropped
    //     4  u32  connection id    the receiver's connection, or the packet is dropped
    //     8  u16  sequence         this packet's number, +1 per packet sent, wraps at 65536
    //    10  u16  ack              the newest packet number received from the other side (valid if flags bit 0)
    //    12  u32  ack bits         bit i set: packet (ack - 1 - i) was received too
    //    16  u8   flags            bit 0: ack and ack bits are valid; bits 1-7 must be zero
    //   blocks, back to back until the end of the datagram (a datagram may have none)
    //     reliable unit (type 1, 6 byte header + data)
    //        u8 type=1, u16 unit id, u8 unit flags (bit 0: first unit of a message, bit 1: last; others zero),
    //        u16 length (1 to kMaxUnitPayload), data
    //     unreliable message (type 2, 5 byte header + data)
    //        u8 type=2, u16 message sequence, u16 length (1 to kMaxUnreliablePayload), data
    //
    // Any other type, a length of 0 or past the end of the datagram or past its cap, a reserved bit set, or bytes
    // left over after the last block makes the whole packet malformed: it is dropped without effect.

    inline constexpr std::size_t kHeaderSize = 17;
    inline constexpr std::size_t kUnitHeaderSize = 6;
    inline constexpr std::size_t kUnreliableHeaderSize = 5;

    inline constexpr std::uint8_t kBlockReliableUnit = 1;
    inline constexpr std::uint8_t kBlockUnreliable = 2;

    inline constexpr std::uint8_t kHeaderFlagHasAck = 0x01;
    inline constexpr std::uint8_t kUnitFlagFirst = 0x01;
    inline constexpr std::uint8_t kUnitFlagLast = 0x02;

    /// The most data one reliable unit carries: a unit fills a whole packet exactly
    inline constexpr std::size_t kMaxUnitPayload = kMaxPacketSize - kHeaderSize - kUnitHeaderSize;
    /// The most one unreliable message carries (it never fragments)
    inline constexpr std::size_t kMaxUnreliablePayload = kMaxPacketSize - kHeaderSize - kUnreliableHeaderSize;

    struct PacketHeader
    {
        std::uint32_t protocolId = 0;
        std::uint32_t connectionId = 0;
        std::uint16_t sequence = 0;
        std::uint16_t ack = 0;
        std::uint32_t ackBits = 0;
        bool hasAck = false;
    };

    /// One parsed block; `data` views into the packet
    struct PacketBlock
    {
        std::uint8_t type = 0;
        /// Unit id, or the unreliable message sequence
        std::uint16_t id = 0;
        /// Unit flags (0 for an unreliable message)
        std::uint8_t flags = 0;
        std::span<const std::uint8_t> data;
    };

    /// Writes the 17 header bytes. False (nothing useful written) if `writer` has no room.
    bool WriteHeader(ByteWriter &writer, const PacketHeader &header) noexcept;

    /// Reads and checks the header: false if truncated or a reserved flag bit is set
    [[nodiscard]] bool ReadHeader(ByteReader &reader, PacketHeader &header) noexcept;

    enum class BlockResult : std::uint8_t
    {
        /// `block` holds the next block
        Block,
        /// The datagram ended exactly after the last block
        End,
        /// The next block is invalid
        Malformed,
    };

    /// Reads the next block from what is left after the header. Every length is checked against the bytes that remain
    /// and against the per-block cap before any data is taken.
    [[nodiscard]] BlockResult ReadBlock(ByteReader &reader, PacketBlock &block) noexcept;

    /// True if `blocks` (the bytes after the header) parse completely: no invalid block, nothing left over
    [[nodiscard]] bool ValidateBlocks(std::span<const std::uint8_t> blocks, std::size_t &blockCount) noexcept;
} // namespace N2Engine::N2Net
