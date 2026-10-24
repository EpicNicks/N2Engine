#include "engine/network/LockstepProtocol.hpp"

#include <algorithm>

#include "engine/network/n2net/BisectProtocol.hpp"
#include "engine/network/n2net/ByteIO.hpp"
#include "engine/network/n2net/Lockstep.hpp"

namespace N2Engine::Network
{
    namespace
    {
        void PutU64(std::vector<std::uint8_t> &out, const std::uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }

        void PutU16(std::vector<std::uint8_t> &out, const std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            out.push_back(static_cast<std::uint8_t>(value >> 8));
        }

        void PutU32(std::vector<std::uint8_t> &out, const std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
            }
        }

        std::uint64_t ReadU64(N2Net::ByteReader &reader) noexcept
        {
            const std::uint64_t low = reader.U32();
            const std::uint64_t high = reader.U32();
            return low | (high << 32);
        }

        bool Starts(const std::span<const std::uint8_t> bytes, const RelayMessage type) noexcept
        {
            return !bytes.empty() && bytes[0] == static_cast<std::uint8_t>(type);
        }
    }

    std::string_view ToString(const RefuseReason reason) noexcept
    {
        switch (reason)
        {
        case RefuseReason::None:
            return "none";
        case RefuseReason::WrongVersion:
            return "wrong-version";
        case RefuseReason::LayoutMismatch:
            return "layout-mismatch";
        case RefuseReason::SessionFull:
            return "session-full";
        case RefuseReason::SessionRunning:
            return "session-running";
        case RefuseReason::Protocol:
            return "protocol";
        }
        return "unknown";
    }

    std::string_view ToString(const DropReason reason) noexcept
    {
        switch (reason)
        {
        case DropReason::Stalled:
            return "stalled";
        case DropReason::Left:
            return "left";
        case DropReason::Violation:
            return "violation";
        }
        return "unknown";
    }

    std::string_view ToString(const StopReason reason) noexcept
    {
        switch (reason)
        {
        case StopReason::Desync:
            return "desync";
        case StopReason::Stalled:
            return "stalled";
        case StopReason::PeerLeft:
            return "peer-left";
        case StopReason::Shutdown:
            return "shutdown";
        case StopReason::Empty:
            return "empty";
        }
        return "unknown";
    }

    std::vector<std::uint8_t> Encode(const JoinMessage &message)
    {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(RelayMessage::Join));
        PutU16(out, message.version);
        out.push_back(message.payloadBytes);
        PutU64(out, message.layoutHash);
        return out;
    }

    std::vector<std::uint8_t> Encode(const WelcomeMessage &message)
    {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(RelayMessage::Welcome));
        PutU16(out, message.version);
        out.push_back(message.slot);
        out.push_back(message.slotCount);
        out.push_back(message.inputDelay);
        PutU16(out, message.tickRate);
        PutU16(out, message.hashInterval);
        out.push_back(message.dropFill);
        out.push_back(message.payloadBytes);
        PutU64(out, message.layoutHash);
        out.insert(out.end(), message.neutral.begin(), message.neutral.end());
        return out;
    }

    std::vector<std::uint8_t> Encode(const DropSlotMessage &message)
    {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(RelayMessage::DropSlot));
        out.push_back(message.slot);
        PutU32(out, message.fromTick);
        out.push_back(static_cast<std::uint8_t>(message.reason));
        return out;
    }

    std::vector<std::uint8_t> Encode(const StopMessage &message)
    {
        std::vector<std::uint8_t> out;
        out.push_back(static_cast<std::uint8_t>(RelayMessage::Stop));
        out.push_back(static_cast<std::uint8_t>(message.reason));
        PutU32(out, message.tick);
        out.push_back(message.slotA);
        out.push_back(message.slotB);
        PutU64(out, message.rootA);
        PutU64(out, message.rootB);
        return out;
    }

    std::vector<std::uint8_t> EncodeStart()
    {
        return {static_cast<std::uint8_t>(RelayMessage::Start)};
    }

    std::vector<std::uint8_t> EncodeRefuse(const RefuseReason reason)
    {
        return {static_cast<std::uint8_t>(RelayMessage::Refuse), static_cast<std::uint8_t>(reason)};
    }

    std::vector<std::uint8_t> EncodePause(const bool paused)
    {
        return {static_cast<std::uint8_t>(RelayMessage::Pause), static_cast<std::uint8_t>(paused ? 1 : 0)};
    }

    std::vector<std::uint8_t> EncodeForward(const RelayMessage type, const std::uint8_t slot,
                                            const std::span<const std::uint8_t> inner)
    {
        if (inner.empty() || inner.size() + 2 > kLockstepMaxMessageBytes)
        {
            return {};
        }
        std::vector<std::uint8_t> out;
        out.reserve(inner.size() + 2);
        out.push_back(static_cast<std::uint8_t>(type));
        out.push_back(slot);
        out.insert(out.end(), inner.begin(), inner.end());
        return out;
    }

    std::optional<JoinMessage> DecodeJoin(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (!Starts(bytes, RelayMessage::Join))
        {
            return std::nullopt;
        }
        N2Net::ByteReader reader(bytes.subspan(1));
        JoinMessage message;
        message.version = reader.U16();
        message.payloadBytes = reader.U8();
        message.layoutHash = ReadU64(reader);
        if (reader.Failed() || reader.Remaining() != 0 || message.payloadBytes > N2Net::kMaxLockstepInputBytes)
        {
            return std::nullopt;
        }
        return message;
    }

    std::optional<WelcomeMessage> DecodeWelcome(const std::span<const std::uint8_t> bytes)
    {
        if (!Starts(bytes, RelayMessage::Welcome))
        {
            return std::nullopt;
        }
        N2Net::ByteReader reader(bytes.subspan(1));
        WelcomeMessage message;
        message.version = reader.U16();
        message.slot = reader.U8();
        message.slotCount = reader.U8();
        message.inputDelay = reader.U8();
        message.tickRate = reader.U16();
        message.hashInterval = reader.U16();
        message.dropFill = reader.U8();
        message.payloadBytes = reader.U8();
        message.layoutHash = ReadU64(reader);
        if (reader.Failed() || message.payloadBytes > N2Net::kMaxLockstepInputBytes ||
            reader.Remaining() != message.payloadBytes)
        {
            return std::nullopt;
        }
        const auto neutral = reader.Bytes(message.payloadBytes);
        message.neutral.assign(neutral.begin(), neutral.end());
        if (message.slotCount < 1 || message.slotCount > kMaxRelaySlots || message.slot >= message.slotCount ||
            message.inputDelay > N2Net::kMaxInputDelay || message.dropFill > 1 || message.tickRate == 0 ||
            message.hashInterval == 0)
        {
            return std::nullopt;
        }
        return message;
    }

    std::optional<DropSlotMessage> DecodeDropSlot(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (!Starts(bytes, RelayMessage::DropSlot))
        {
            return std::nullopt;
        }
        N2Net::ByteReader reader(bytes.subspan(1));
        DropSlotMessage message;
        message.slot = reader.U8();
        message.fromTick = reader.U32();
        const std::uint8_t reason = reader.U8();
        if (reader.Failed() || reader.Remaining() != 0 || message.slot >= kMaxRelaySlots || reason < 1 || reason > 3)
        {
            return std::nullopt;
        }
        message.reason = static_cast<DropReason>(reason);
        return message;
    }

    std::optional<StopMessage> DecodeStop(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (!Starts(bytes, RelayMessage::Stop))
        {
            return std::nullopt;
        }
        N2Net::ByteReader reader(bytes.subspan(1));
        StopMessage message;
        const std::uint8_t reason = reader.U8();
        message.tick = reader.U32();
        message.slotA = reader.U8();
        message.slotB = reader.U8();
        message.rootA = ReadU64(reader);
        message.rootB = ReadU64(reader);
        if (reader.Failed() || reader.Remaining() != 0 || reason < 1 || reason > 5 || message.slotA >= kMaxRelaySlots ||
            message.slotB >= kMaxRelaySlots)
        {
            return std::nullopt;
        }
        message.reason = static_cast<StopReason>(reason);
        return message;
    }

    std::optional<RefuseReason> DecodeRefuse(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (!Starts(bytes, RelayMessage::Refuse) || bytes.size() != 2 || bytes[1] < 1 || bytes[1] > 5)
        {
            return std::nullopt;
        }
        return static_cast<RefuseReason>(bytes[1]);
    }

    std::optional<bool> DecodePause(const std::span<const std::uint8_t> bytes) noexcept
    {
        if (!Starts(bytes, RelayMessage::Pause) || bytes.size() != 2 || bytes[1] > 1)
        {
            return std::nullopt;
        }
        return bytes[1] == 1;
    }

    std::optional<ForwardedMessage> DecodeForward(const RelayMessage type, const std::span<const std::uint8_t> bytes) noexcept
    {
        if (!Starts(bytes, type) || bytes.size() < 3 || bytes.size() > 2 + N2Net::kMaxBisectReplySize ||
            bytes[1] >= kMaxRelaySlots)
        {
            return std::nullopt;
        }
        const std::span<const std::uint8_t> inner = bytes.subspan(2);
        const std::uint8_t kind = N2Net::PeekMessageKind(inner);
        if (kind != N2Net::kBisectRequestKind && kind != N2Net::kBisectReplyKind)
        {
            return std::nullopt;
        }
        return ForwardedMessage{bytes[1], inner};
    }

    N2Net::ConnectionConfig MakeLockstepConnectionConfig(const std::uint32_t protocolId, const N2Net::ITransport &transport)
    {
        N2Net::ConnectionConfig config;
        config.protocolId = protocolId;
        config.connectionId = kLockstepConnectionId;
        config.reliableStream = transport.IsReliableStream();
        config.maxPacketSize = static_cast<std::uint32_t>(std::min(transport.MaxPacketSize(), N2Net::kMaxPacketSize));
        config.maxMessageSize = static_cast<std::uint32_t>(kLockstepMaxMessageBytes);
        return config;
    }
}
