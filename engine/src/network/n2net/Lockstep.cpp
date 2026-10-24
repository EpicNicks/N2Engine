#include "engine/network/n2net/Lockstep.hpp"

#include <algorithm>
#include <string>

#include "engine/network/n2net/ByteIO.hpp"

namespace N2Engine::N2Net
{
    namespace
    {
        constexpr std::int32_t Diff(const std::uint32_t a, const std::uint32_t b) noexcept
        {
            return static_cast<std::int32_t>(a - b);
        }
    }

    std::optional<LockstepInput> LockstepInput::FromBytes(const std::span<const std::uint8_t> payload) noexcept
    {
        if (payload.size() > kMaxLockstepInputBytes)
        {
            return std::nullopt;
        }
        LockstepInput input;
        std::copy(payload.begin(), payload.end(), input.bytes.begin());
        input.size = static_cast<std::uint8_t>(payload.size());
        return input;
    }

    LockstepScheduler::LockstepScheduler(const LockstepConfig &config) : config_(config)
    {
        // The neutral input must be a payload of the layout's size, or the layout info is inconsistent
        if (config.layout.payloadBytes > kMaxLockstepInputBytes || config.layout.neutral.size != config.layout.payloadBytes)
        {
            return;
        }
        payloadBytes_ = config.layout.payloadBytes;
        layoutHash_ = config.layout.hash;
        neutral_ = config.layout.neutral;
        if (config.slotCount < 1 || config.slotCount > static_cast<std::uint32_t>(kMaxLockstepSlots) ||
            config.inputDelay > kMaxInputDelay)
        {
            return;
        }
        valid_ = true;
        current_ = config.startTick;
        for (std::uint32_t s = 0; s < config.slotCount; ++s)
        {
            Slot &slot = slots_[s];
            slot.lastInput = neutral_;
            slot.nextLocalTick = current_ + config.inputDelay;
            // The ticks no one can send for are known neutral input
            for (std::uint32_t d = 0; d < config.inputDelay; ++d)
            {
                const std::uint32_t tick = current_ + d;
                Entry &entry = slot.ring[tick % kLockstepWindow];
                entry.tick = tick;
                entry.valid = true;
                entry.input = neutral_;
            }
        }
    }

    bool LockstepScheduler::Lookup(const Slot &slot, const std::uint32_t tick, LockstepInput &out) noexcept
    {
        const Entry &entry = slot.ring[tick % kLockstepWindow];
        if (!entry.valid || entry.tick != tick)
        {
            return false;
        }
        out = entry.input;
        return true;
    }

    bool LockstepScheduler::IsDropped(const std::uint32_t slot, const std::uint32_t tick) const noexcept
    {
        if (!valid_ || slot >= config_.slotCount)
        {
            return false;
        }
        const Slot &s = slots_[slot];
        return s.dropped && Diff(tick, s.droppedFrom) >= 0;
    }

    SubmitResult LockstepScheduler::Submit(const std::uint32_t slot, const std::uint32_t tick, const LockstepInput &input) noexcept
    {
        if (!valid_ || slot >= config_.slotCount)
        {
            return SubmitResult::BadSlot;
        }
        const std::int32_t ahead = Diff(tick, current_);
        if (ahead < 0)
        {
            return SubmitResult::TooOld;
        }
        if (ahead >= static_cast<std::int32_t>(kLockstepWindow))
        {
            return SubmitResult::TooFar;
        }
        if (IsDropped(slot, tick))
        {
            return SubmitResult::SlotDropped;
        }
        if (input.size != payloadBytes_)
        {
            return SubmitResult::BadPayload;
        }
        Entry &entry = slots_[slot].ring[tick % kLockstepWindow];
        if (entry.valid && entry.tick == tick)
        {
            return entry.input == input ? SubmitResult::Duplicate : SubmitResult::Conflict;
        }
        entry.tick = tick;
        entry.valid = true;
        entry.input = input;
        return SubmitResult::Accepted;
    }

    SubmitResult LockstepScheduler::SubmitLocal(const std::uint32_t slot, const LockstepInput &input,
                                                std::uint32_t &assignedTick) noexcept
    {
        if (!valid_ || slot >= config_.slotCount)
        {
            return SubmitResult::BadSlot;
        }
        Slot &s = slots_[slot];
        const SubmitResult result = Submit(slot, s.nextLocalTick, input);
        if (result == SubmitResult::Accepted)
        {
            assignedTick = s.nextLocalTick;
            ++s.nextLocalTick;
        }
        return result;
    }

    std::uint32_t LockstepScheduler::MissingSlots() const noexcept
    {
        std::uint32_t mask = 0;
        if (!valid_)
        {
            return mask;
        }
        for (std::uint32_t s = 0; s < config_.slotCount; ++s)
        {
            LockstepInput ignored;
            if (!IsDropped(s, current_) && !Lookup(slots_[s], current_, ignored))
            {
                mask |= 1u << s;
            }
        }
        return mask;
    }

    AdvanceStatus LockstepScheduler::TryAdvance(TickInputs &out) noexcept
    {
        if (!valid_)
        {
            return AdvanceStatus::Waiting;
        }
        if (MissingSlots() != 0)
        {
            if (stalledPolls_ < 0xFFFFFFFFu)
            {
                ++stalledPolls_;
            }
            return stalledPolls_ >= config_.stallTimeoutPolls ? AdvanceStatus::TimedOut : AdvanceStatus::Waiting;
        }
        out.tick = current_;
        out.inputs = {};
        for (std::uint32_t s = 0; s < config_.slotCount; ++s)
        {
            Slot &slot = slots_[s];
            LockstepInput input;
            if (IsDropped(s, current_))
            {
                input = config_.dropFill == DropFill::HoldLast ? slot.lastInput : neutral_;
            }
            else
            {
                (void)Lookup(slot, current_, input);
            }
            slot.lastInput = input;
            out.inputs[s] = input;
        }
        ++current_;
        stalledPolls_ = 0;
        return AdvanceStatus::Runnable;
    }

    std::uint32_t LockstepScheduler::NextNeeded(const std::uint32_t slot) const noexcept
    {
        if (!valid_ || slot >= config_.slotCount)
        {
            return 0;
        }
        std::uint32_t tick = current_;
        for (std::uint32_t i = 0; i < kLockstepWindow; ++i, ++tick)
        {
            LockstepInput ignored;
            if (!IsDropped(slot, tick) && !Lookup(slots_[slot], tick, ignored))
            {
                break;
            }
        }
        return tick;
    }

    std::optional<LockstepInput> LockstepScheduler::Get(const std::uint32_t slot, const std::uint32_t tick) const noexcept
    {
        if (!valid_ || slot >= config_.slotCount)
        {
            return std::nullopt;
        }
        LockstepInput input;
        if (!Lookup(slots_[slot], tick, input))
        {
            return std::nullopt;
        }
        return input;
    }

    bool LockstepScheduler::DropSlot(const std::uint32_t slot, const std::uint32_t fromTick) noexcept
    {
        if (!valid_ || slot >= config_.slotCount || Diff(fromTick, current_) < 0)
        {
            return false;
        }
        Slot &s = slots_[slot];
        if (s.dropped)
        {
            return false;
        }
        s.dropped = true;
        s.droppedFrom = fromTick;
        return true;
    }

    std::size_t EncodeInputBundle(const InputBundle &bundle, const std::span<std::uint8_t> out) noexcept
    {
        if (bundle.frames.size() > kMaxBundleFrames || bundle.payloadBytes > kMaxLockstepInputBytes ||
            out.size() < InputBundleSize(bundle.frames.size(), bundle.payloadBytes))
        {
            return 0;
        }
        for (const LockstepInput &frame : bundle.frames)
        {
            if (frame.size != bundle.payloadBytes)
            {
                return 0;
            }
        }
        ByteWriter writer(out.data(), out.size());
        writer.U8(1); // message kind: input bundle
        writer.U8(bundle.slot);
        writer.U32(bundle.ackTick);
        writer.U32(bundle.firstTick);
        writer.U8(static_cast<std::uint8_t>(bundle.frames.size()));
        writer.U8(bundle.payloadBytes);
        writer.U32(static_cast<std::uint32_t>(bundle.layoutHash & 0xFFFFFFFFu));
        writer.U32(static_cast<std::uint32_t>(bundle.layoutHash >> 32));
        for (const LockstepInput &frame : bundle.frames)
        {
            writer.Bytes(frame.Bytes());
        }
        return writer.Overflowed() ? 0 : writer.Size();
    }

    std::expected<InputBundle, BundleError> DecodeInputBundle(const std::span<const std::uint8_t> bytes,
                                                              const LockstepLayoutInfo &layout)
    {
        ByteReader reader(bytes);
        if (reader.U8() != 1)
        {
            return std::unexpected(BundleError::Malformed);
        }
        InputBundle bundle;
        bundle.slot = reader.U8();
        bundle.ackTick = reader.U32();
        bundle.firstTick = reader.U32();
        const std::size_t count = reader.U8();
        bundle.payloadBytes = reader.U8();
        const std::uint64_t low = reader.U32();
        const std::uint64_t high = reader.U32();
        bundle.layoutHash = low | (high << 32);
        // The counts are checked against the caps and against the bytes that are there before anything is reserved
        if (reader.Failed() || count > kMaxBundleFrames || bundle.payloadBytes > kMaxLockstepInputBytes ||
            reader.Remaining() != count * bundle.payloadBytes)
        {
            return std::unexpected(BundleError::Malformed);
        }
        if (bundle.layoutHash != layout.hash || bundle.payloadBytes != layout.payloadBytes)
        {
            return std::unexpected(BundleError::LayoutMismatch);
        }
        bundle.frames.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const std::span<const std::uint8_t> payload = reader.Bytes(bundle.payloadBytes);
            const std::optional<LockstepInput> frame = LockstepInput::FromBytes(payload);
            if (reader.Failed() || !frame)
            {
                return std::unexpected(BundleError::Malformed);
            }
            bundle.frames.push_back(*frame);
        }
        return bundle;
    }

    InputBundle BuildBundleFor(const LockstepScheduler &scheduler, const std::uint32_t slot, const std::uint32_t peerAck)
    {
        InputBundle bundle;
        bundle.payloadBytes = static_cast<std::uint8_t>(scheduler.PayloadBytes());
        bundle.layoutHash = scheduler.LayoutHash();
        bundle.slot = static_cast<std::uint8_t>(slot);
        bundle.ackTick = scheduler.CurrentTick();
        bundle.firstTick = peerAck;
        if (!scheduler.Valid() || slot >= scheduler.Config().slotCount)
        {
            return bundle;
        }
        std::uint32_t tick = peerAck;
        const std::uint32_t oldest = scheduler.CurrentTick() - (kLockstepWindow - kMaxInputDelay);
        if (Diff(tick, oldest) < 0)
        {
            tick = oldest;
        }
        bundle.firstTick = tick;
        while (bundle.frames.size() < kMaxBundleFrames)
        {
            const std::optional<LockstepInput> input = scheduler.Get(slot, tick);
            if (!input)
            {
                break;
            }
            bundle.frames.push_back(*input);
            ++tick;
        }
        return bundle;
    }
}
