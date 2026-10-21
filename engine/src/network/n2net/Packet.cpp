#include "engine/network/n2net/Packet.hpp"

namespace N2Engine::N2Net
{
    bool WriteHeader(ByteWriter &writer, const PacketHeader &header) noexcept
    {
        writer.U32(header.protocolId);
        writer.U32(header.connectionId);
        writer.U16(header.sequence);
        writer.U16(header.hasAck ? header.ack : static_cast<std::uint16_t>(0));
        writer.U32(header.hasAck ? header.ackBits : 0u);
        writer.U8(header.hasAck ? kHeaderFlagHasAck : static_cast<std::uint8_t>(0));
        return !writer.Overflowed();
    }

    bool ReadHeader(ByteReader &reader, PacketHeader &header) noexcept
    {
        header.protocolId = reader.U32();
        header.connectionId = reader.U32();
        header.sequence = reader.U16();
        header.ack = reader.U16();
        header.ackBits = reader.U32();
        const std::uint8_t flags = reader.U8();
        if (reader.Failed() || (flags & ~kHeaderFlagHasAck) != 0)
        {
            return false;
        }
        header.hasAck = (flags & kHeaderFlagHasAck) != 0;
        return true;
    }

    BlockResult ReadBlock(ByteReader &reader, PacketBlock &block) noexcept
    {
        if (reader.Remaining() == 0)
        {
            return BlockResult::End;
        }

        const std::uint8_t type = reader.U8();
        std::size_t maxLength = 0;
        block = PacketBlock{};
        block.type = type;
        if (type == kBlockReliableUnit)
        {
            block.id = reader.U16();
            block.flags = reader.U8();
            if ((block.flags & ~(kUnitFlagFirst | kUnitFlagLast)) != 0)
            {
                return BlockResult::Malformed;
            }
            maxLength = kMaxUnitPayload;
        }
        else if (type == kBlockUnreliable)
        {
            block.id = reader.U16();
            maxLength = kMaxUnreliablePayload;
        }
        else
        {
            return BlockResult::Malformed;
        }

        const std::size_t length = reader.U16();
        if (reader.Failed() || length == 0 || length > maxLength || length > reader.Remaining())
        {
            return BlockResult::Malformed;
        }
        block.data = reader.Bytes(length);
        return reader.Failed() ? BlockResult::Malformed : BlockResult::Block;
    }

    bool ValidateBlocks(std::span<const std::uint8_t> blocks, std::size_t &blockCount) noexcept
    {
        ByteReader reader(blocks);
        PacketBlock block;
        blockCount = 0;
        for (;;)
        {
            const BlockResult result = ReadBlock(reader, block);
            if (result == BlockResult::End)
            {
                return true;
            }
            if (result == BlockResult::Malformed)
            {
                return false;
            }
            ++blockCount;
        }
    }
} // namespace N2Engine::N2Net
