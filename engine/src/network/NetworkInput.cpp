#include "engine/network/NetworkInput.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "engine/network/n2net/ByteIO.hpp"

namespace N2Engine::Network
{
    namespace
    {
        using N2Net::ByteReader;
        using N2Net::ByteWriter;

        constexpr std::uint32_t kHistoryMask = kInputHistoryTicks - 1;
        constexpr std::uint32_t kWindowMask = kInputWindowTicks - 1;
        constexpr std::uint32_t kAckBitCount = 32;
    }

    std::string_view ToString(const InputSource source) noexcept
    {
        switch (source)
        {
        case InputSource::Frame:
            return "frame";
        case InputSource::Repeated:
            return "repeated";
        case InputSource::Neutral:
            return "neutral";
        }
        return "unknown";
    }

    // ---- Wire --------------------------------------------------------------------------------------------------------

    std::vector<std::uint8_t> EncodeInputFrames(const std::span<const InputFrame> frames, const std::size_t payloadBytes)
    {
        if (frames.empty() || frames.size() > kMaxInputFramesPerMessage || payloadBytes > kMaxInputPayloadBytes)
        {
            return {};
        }
        for (const InputFrame &frame : frames)
        {
            if (frame.payload.size() != payloadBytes)
            {
                return {};
            }
        }
        std::vector<std::uint8_t> bytes(2 + frames.size() * (kInputFrameHeaderBytes + payloadBytes));
        ByteWriter writer(bytes.data(), bytes.size());
        writer.U8(kInputFramesType);
        writer.U8(static_cast<std::uint8_t>(frames.size()));
        for (const InputFrame &frame : frames)
        {
            writer.U32(frame.tick);
            writer.U32(frame.sequence);
            writer.Bytes(frame.payload);
        }
        // Cannot overflow: the buffer is exactly the size of what was written
        bytes.resize(writer.Size());
        return bytes;
    }

    std::expected<std::vector<InputFrame>, DecodeError> DecodeInputFrames(const std::span<const std::uint8_t> bytes,
                                                                          const std::size_t payloadBytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        if (payloadBytes > kMaxInputPayloadBytes)
        {
            return std::unexpected(DecodeError::BadValue);
        }
        ByteReader reader(bytes);
        const std::uint8_t type = reader.U8();
        if (type != kInputFramesType)
        {
            return std::unexpected(DecodeError::UnknownType);
        }
        const std::size_t count = reader.U8();
        if (reader.Failed())
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (count == 0 || count > kMaxInputFramesPerMessage)
        {
            return std::unexpected(DecodeError::BadValue);
        }
        // The length is known from the count and the layout, before anything is taken
        const std::size_t expectedBytes = 2 + count * (kInputFrameHeaderBytes + payloadBytes);
        if (bytes.size() < expectedBytes)
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (bytes.size() > expectedBytes)
        {
            return std::unexpected(DecodeError::TrailingBytes);
        }

        std::vector<InputFrame> frames;
        frames.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            InputFrame frame;
            // Separate statements: the order of two reads in one expression is unspecified
            frame.tick = reader.U32();
            frame.sequence = reader.U32();
            const std::span<const std::uint8_t> payload = reader.Bytes(payloadBytes);
            if (reader.Failed())
            {
                return std::unexpected(DecodeError::Truncated);
            }
            frame.payload.assign(payload.begin(), payload.end());
            if (frame.sequence == 0)
            {
                return std::unexpected(DecodeError::BadValue);
            }
            if (!frames.empty() && (TickDifference(frame.tick, frames.back().tick) <= 0 ||
                                    TickDifference(frame.sequence, frames.back().sequence) <= 0))
            {
                return std::unexpected(DecodeError::BadValue);
            }
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    std::vector<std::uint8_t> Encode(const InputAckMessage &message)
    {
        std::vector<std::uint8_t> bytes(9);
        ByteWriter writer(bytes.data(), bytes.size());
        writer.U8(kInputAckType);
        writer.U32(message.latest);
        writer.U32(message.bits);
        return bytes;
    }

    std::expected<InputAckMessage, DecodeError> DecodeInputAck(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty())
        {
            return std::unexpected(DecodeError::Empty);
        }
        if (bytes[0] != kInputAckType)
        {
            return std::unexpected(DecodeError::UnknownType);
        }
        if (bytes.size() < 9)
        {
            return std::unexpected(DecodeError::Truncated);
        }
        if (bytes.size() > 9)
        {
            return std::unexpected(DecodeError::TrailingBytes);
        }
        ByteReader reader(bytes);
        (void)reader.U8();
        InputAckMessage message;
        message.latest = reader.U32();
        message.bits = reader.U32();
        if (message.latest == 0)
        {
            return std::unexpected(DecodeError::BadValue);
        }
        return message;
    }

    // ---- Server ------------------------------------------------------------------------------------------------------

    ServerInputOptions ServerInputOptions::Clamped() const noexcept
    {
        ServerInputOptions out = *this;
        out.delayTicks = std::min(out.delayTicks, kMaxInputDelayTicks);
        out.maxFutureTicks = std::clamp<std::uint32_t>(out.maxFutureTicks, 1u, kMaxInputFutureTicks);
        out.repeatLastTicks = std::min(out.repeatLastTicks, kMaxInputRepeatTicks);
        return out;
    }

    void ServerInputStats::Add(const ServerInputStats &other) noexcept
    {
        framesReceived += other.framesReceived;
        framesAccepted += other.framesAccepted;
        framesDuplicate += other.framesDuplicate;
        framesLate += other.framesLate;
        framesTooFar += other.framesTooFar;
        framesInvalid += other.framesInvalid;
        ticksFromFrames += other.ticksFromFrames;
        ticksRepeated += other.ticksRepeated;
        ticksNeutral += other.ticksNeutral;
        ticksSkipped += other.ticksSkipped;
    }

    ServerInputBuffer::ServerInputBuffer(const InputLayout &layout, const ServerInputOptions &options,
                                         const std::uint32_t startTick)
        : _layout(&layout),
          _options(options.Clamped()),
          _payloadBytes(layout.PayloadBytes()),
          _now(startTick),
          _cursor(startTick)
    {
        const std::vector<std::uint8_t> neutral = layout.NeutralPayload();
        std::copy_n(neutral.begin(), std::min(neutral.size(), _neutral.size()), _neutral.begin());
    }

    bool ServerInputBuffer::SequenceSeen(const std::uint32_t sequence) const noexcept
    {
        if (_ackLatest == 0)
        {
            return false;
        }
        const std::int32_t behind = TickDifference(_ackLatest, sequence);
        if (behind < 0)
        {
            return false;
        }
        if (behind == 0)
        {
            return true;
        }
        if (behind > static_cast<std::int32_t>(kAckBitCount))
        {
            // Older than the acknowledgement can say: not worth keeping
            return true;
        }
        return ((_ackBits >> (behind - 1)) & 1u) != 0;
    }

    void ServerInputBuffer::MarkSeen(const std::uint32_t sequence) noexcept
    {
        if (_ackLatest == 0)
        {
            _ackLatest = sequence;
            _ackBits = 0;
        }
        else
        {
            const std::int32_t difference = TickDifference(sequence, _ackLatest);
            if (difference > 0)
            {
                const std::uint32_t delta = sequence - _ackLatest;
                if (delta > kAckBitCount)
                {
                    _ackBits = 0;
                }
                else
                {
                    const std::uint64_t widened =
                        (static_cast<std::uint64_t>(_ackBits) << delta) | (std::uint64_t{1} << (delta - 1));
                    _ackBits = static_cast<std::uint32_t>(widened & 0xFFFFFFFFull);
                }
                _ackLatest = sequence;
            }
            else if (difference < 0)
            {
                const std::uint32_t back = _ackLatest - sequence;
                if (back > kAckBitCount)
                {
                    return;
                }
                _ackBits |= std::uint32_t{1} << (back - 1);
            }
            else
            {
                return;
            }
        }
        _ackPending = true;
    }

    InputReceive ServerInputBuffer::Receive(const InputFrame &frame)
    {
        ++_stats.framesReceived;
        if (frame.sequence == 0 || !_layout->IsValidPayload(frame.payload))
        {
            ++_stats.framesInvalid;
            return InputReceive::Invalid;
        }
        if (SequenceSeen(frame.sequence))
        {
            ++_stats.framesDuplicate;
            return InputReceive::Duplicate;
        }

        const std::uint32_t applyTick = frame.tick + _options.delayTicks;
        if (TickBefore(applyTick, _cursor))
        {
            // The tick was applied already, from the missing-input policy; the client need not send this again
            MarkSeen(frame.sequence);
            ++_stats.framesLate;
            return InputReceive::Late;
        }
        if (TickDifference(applyTick, _now) > static_cast<std::int32_t>(_options.maxFutureTicks))
        {
            // Not acknowledged: the client keeps it and sends it again, by when it may be in range
            ++_stats.framesTooFar;
            return InputReceive::TooFar;
        }

        Slot &slot = _slots[applyTick & kWindowMask];
        if (slot.used && slot.tick == applyTick)
        {
            MarkSeen(frame.sequence);
            ++_stats.framesDuplicate;
            return InputReceive::Duplicate;
        }
        if (!slot.used)
        {
            ++_buffered;
        }
        slot.used = true;
        slot.tick = applyTick;
        slot.frameTick = frame.tick;
        slot.payload = {};
        std::copy_n(frame.payload.begin(), std::min(_payloadBytes, slot.payload.size()), slot.payload.begin());
        MarkSeen(frame.sequence);
        ++_stats.framesAccepted;
        return InputReceive::Accepted;
    }

    void ServerInputBuffer::SetNow(const std::uint32_t tick)
    {
        _now = tick;
        if (TickDifference(_now, _cursor) > static_cast<std::int32_t>(kInputHistoryTicks))
        {
            SkipTo(_now - kInputHistoryTicks);
        }
        else if (TickDifference(_cursor, _now) > static_cast<std::int32_t>(kInputHistoryTicks * 2))
        {
            // The server's tick counter went back (a scene restart): start over from it
            Resync(_now);
        }
    }

    void ServerInputBuffer::SkipTo(const std::uint32_t newCursor)
    {
        const std::uint32_t count = newCursor - _cursor;
        _stats.ticksSkipped += count;
        const std::uint64_t streak = static_cast<std::uint64_t>(_missingStreak) + count;
        _missingStreak = static_cast<std::uint32_t>(std::min<std::uint64_t>(streak, 0xFFFFFFFFull));
        for (Slot &slot : _slots)
        {
            if (slot.used && TickBefore(slot.tick, newCursor))
            {
                slot.used = false;
                --_buffered;
            }
        }
        _cursor = newCursor;
    }

    void ServerInputBuffer::Resync(const std::uint32_t now)
    {
        for (Slot &slot : _slots)
        {
            slot.used = false;
        }
        for (HistoryEntry &entry : _history)
        {
            entry.valid = false;
        }
        _buffered = 0;
        _missingStreak = 0;
        _cursor = now;
    }

    void ServerInputBuffer::StepOne()
    {
        Slot &slot = _slots[_cursor & kWindowMask];
        HistoryEntry &entry = _history[_cursor & kHistoryMask];
        entry.valid = true;
        entry.tick = _cursor;
        if (slot.used && slot.tick == _cursor)
        {
            entry.source = InputSource::Frame;
            entry.payload = slot.payload;
            entry.frameTick = slot.frameTick;
            entry.hasFrameTick = true;
            _last = slot.payload;
            _lastFrameTick = slot.frameTick;
            _haveLast = true;
            _missingStreak = 0;
            slot.used = false;
            --_buffered;
            ++_stats.ticksFromFrames;
        }
        else if (_haveLast && _missingStreak < _options.repeatLastTicks)
        {
            entry.source = InputSource::Repeated;
            entry.payload = _last;
            entry.frameTick = _lastFrameTick;
            entry.hasFrameTick = true;
            ++_missingStreak;
            ++_stats.ticksRepeated;
        }
        else
        {
            entry.source = InputSource::Neutral;
            entry.payload = _neutral;
            entry.hasFrameTick = false;
            entry.frameTick = 0;
            if (_missingStreak < 0xFFFFFFFFu)
            {
                ++_missingStreak;
            }
            ++_stats.ticksNeutral;
        }
        ++_cursor;
    }

    ResolvedInput ServerInputBuffer::ToResolved(const HistoryEntry &entry) const
    {
        ResolvedInput resolved;
        resolved.tick = entry.tick;
        resolved.source = entry.source;
        resolved.frameTick = entry.frameTick;
        resolved.hasFrameTick = entry.hasFrameTick;
        resolved.payload.assign(entry.payload.begin(),
                                entry.payload.begin() + static_cast<std::ptrdiff_t>(_payloadBytes));
        return resolved;
    }

    std::optional<ResolvedInput> ServerInputBuffer::Resolve(const std::uint32_t tick)
    {
        if (TickBefore(tick, _cursor))
        {
            const HistoryEntry &entry = _history[tick & kHistoryMask];
            if (entry.valid && entry.tick == tick)
            {
                return ToResolved(entry);
            }
            return std::nullopt;
        }
        if (TickDifference(tick, _cursor) >= static_cast<std::int32_t>(kInputHistoryTicks))
        {
            SkipTo(tick - (kInputHistoryTicks - 1));
        }
        while (!TickBefore(tick, _cursor))
        {
            StepOne();
        }
        return ToResolved(_history[tick & kHistoryMask]);
    }

    // ---- Client ------------------------------------------------------------------------------------------------------

    ClientInputSender::ClientInputSender(InputLayout layout, const std::uint32_t redundancy)
        : _layout(std::move(layout)),
          _redundancy(std::clamp<std::uint32_t>(redundancy, 1u, static_cast<std::uint32_t>(kMaxInputFramesPerMessage)))
    {
    }

    std::expected<std::uint32_t, std::string> ClientInputSender::Queue(const std::uint32_t tick,
                                                                       const std::span<const float> values)
    {
        if (_layout.Empty())
        {
            return std::unexpected(std::string("this client has no input layout"));
        }
        if (values.size() != _layout.ValueCount())
        {
            return std::unexpected(std::format("the input layout holds {} value(s) and {} were given",
                                               _layout.ValueCount(), values.size()));
        }
        if (_haveTick && TickDifference(tick, _lastTick) <= 0)
        {
            return std::unexpected(std::format("input for tick {} was queued after tick {}: ticks must rise", tick, _lastTick));
        }
        std::optional<std::vector<std::uint8_t>> payload = _layout.Pack(values);
        if (!payload.has_value())
        {
            return std::unexpected(std::string("the input could not be packed"));
        }

        InputFrame frame;
        frame.tick = tick;
        frame.sequence = _nextSequence;
        frame.payload = std::move(*payload);
        ++_nextSequence;
        if (_nextSequence == 0)
        {
            _nextSequence = 1;
        }
        _ring.push_back(std::move(frame));
        if (_ring.size() > kMaxUnackedInputFrames)
        {
            _ring.pop_front();
            ++_stats.framesDroppedFull;
        }
        _lastTick = tick;
        _haveTick = true;
        _pending = true;
        ++_stats.framesQueued;
        return _ring.back().sequence;
    }

    std::optional<std::vector<std::uint8_t>> ClientInputSender::BuildMessage(const std::size_t maxMessageBytes)
    {
        _pending = false;
        if (_ring.empty())
        {
            return std::nullopt;
        }
        const std::size_t payloadBytes = _layout.PayloadBytes();
        const std::size_t perFrame = kInputFrameHeaderBytes + payloadBytes;
        const std::size_t fit = maxMessageBytes > 2 ? (maxMessageBytes - 2) / perFrame : 0;
        const std::size_t count = std::min({static_cast<std::size_t>(_redundancy), kMaxInputFramesPerMessage, fit, _ring.size()});
        if (count == 0)
        {
            ++_stats.messagesTooLarge;
            return std::nullopt;
        }

        // The newest `count`, oldest first
        std::vector<InputFrame> frames(_ring.end() - static_cast<std::ptrdiff_t>(count), _ring.end());
        std::vector<std::uint8_t> message = EncodeInputFrames(frames, payloadBytes);
        if (message.empty())
        {
            return std::nullopt;
        }
        const std::uint32_t newest = frames.back().sequence;
        if (_highestSent == 0 || TickDifference(newest, _highestSent) > 0)
        {
            _highestSent = newest;
        }
        ++_stats.messagesBuilt;
        _stats.framesSent += count;
        return message;
    }

    bool ClientInputSender::OnAck(const InputAckMessage &ack)
    {
        if (_highestSent == 0 || ack.latest == 0 || TickDifference(ack.latest, _highestSent) > 0)
        {
            ++_stats.acksInvalid;
            return false;
        }
        for (auto it = _ring.begin(); it != _ring.end();)
        {
            const std::int32_t behind = TickDifference(ack.latest, it->sequence);
            bool remove = false;
            if (behind == 0)
            {
                remove = true;
                ++_stats.framesAcked;
            }
            else if (behind > 0 && behind <= static_cast<std::int32_t>(kAckBitCount))
            {
                if (((ack.bits >> (behind - 1)) & 1u) != 0)
                {
                    remove = true;
                    ++_stats.framesAcked;
                }
            }
            else if (behind > static_cast<std::int32_t>(kAckBitCount))
            {
                remove = true;
                ++_stats.framesAbandoned;
            }
            it = remove ? _ring.erase(it) : it + 1;
        }
        return true;
    }
}
