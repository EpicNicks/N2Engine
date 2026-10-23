#include "engine/network/n2net/Lockstep.hpp"

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

    LockstepScheduler::LockstepScheduler(const LockstepConfig &config) noexcept : config_(config)
    {
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
            slot.nextLocalTick = current_ + config.inputDelay;
            // The ticks no one can send for are known neutral input
            for (std::uint32_t d = 0; d < config.inputDelay; ++d)
            {
                const std::uint32_t tick = current_ + d;
                Entry &entry = slot.ring[tick % kLockstepWindow];
                entry.tick = tick;
                entry.valid = true;
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
                input = config_.dropFill == DropFill::HoldLast ? slot.lastInput : LockstepInput{};
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
        if (bundle.frames.size() > kMaxBundleFrames || out.size() < InputBundleSize(bundle.frames.size()))
        {
            return 0;
        }
        ByteWriter writer(out.data(), out.size());
        writer.U8(1); // message kind: input bundle
        writer.U8(bundle.slot);
        writer.U32(bundle.ackTick);
        writer.U32(bundle.firstTick);
        writer.U8(static_cast<std::uint8_t>(bundle.frames.size()));
        for (const LockstepInput &frame : bundle.frames)
        {
            writer.U8(static_cast<std::uint8_t>(frame.moveX));
            writer.U8(static_cast<std::uint8_t>(frame.moveY));
            writer.U32(frame.buttons);
        }
        return writer.Overflowed() ? 0 : writer.Size();
    }

    std::optional<InputBundle> DecodeInputBundle(const std::span<const std::uint8_t> bytes)
    {
        ByteReader reader(bytes);
        if (reader.U8() != 1)
        {
            return std::nullopt;
        }
        InputBundle bundle;
        bundle.slot = reader.U8();
        bundle.ackTick = reader.U32();
        bundle.firstTick = reader.U32();
        const std::size_t count = reader.U8();
        // The count is checked against the cap and against the bytes that are there before anything is reserved
        if (reader.Failed() || count > kMaxBundleFrames || reader.Remaining() != count * 6)
        {
            return std::nullopt;
        }
        bundle.frames.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            LockstepInput frame;
            frame.moveX = static_cast<std::int8_t>(reader.U8());
            frame.moveY = static_cast<std::int8_t>(reader.U8());
            frame.buttons = reader.U32();
            bundle.frames.push_back(frame);
        }
        if (reader.Failed())
        {
            return std::nullopt;
        }
        return bundle;
    }

    InputBundle BuildBundleFor(const LockstepScheduler &scheduler, const std::uint32_t slot, const std::uint32_t peerAck)
    {
        InputBundle bundle;
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
