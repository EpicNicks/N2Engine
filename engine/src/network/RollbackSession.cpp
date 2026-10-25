#include "engine/network/RollbackSession.hpp"

#include <algorithm>
#include <format>

namespace N2Engine::Network
{
    RollbackSession::RollbackSession(const RollbackConfig &config, RollbackHost host)
        : config_(config), host_(std::move(host)), events_(config.eventPolicy)
    {
        valid_ = config.slotCount >= 1 && config.slotCount <= static_cast<std::uint32_t>(N2Net::kMaxLockstepSlots) &&
                 config.localSlot < config.slotCount && config.inputDelay <= N2Net::kMaxInputDelay &&
                 config.rollbackWindow <= kMaxRollbackWindow && config.layout.payloadBytes <= N2Net::kMaxLockstepInputBytes &&
                 host_.step != nullptr && (config.rollbackWindow == 0 || (host_.save != nullptr && host_.restore != nullptr));
        if (!valid_)
        {
            failed_ = true;
            error_ = "invalid rollback session configuration";
            return;
        }
        if (config_.rollbackWindow > 0)
        {
            ring_.resize(config_.rollbackWindow + 2);
            ringValid_.assign(ring_.size(), false);
        }
        // The first inputDelay ticks have no sender: every slot's input is neutral, as in lockstep
        for (std::uint32_t tick = 0; tick < config_.inputDelay; ++tick)
        {
            for (std::uint32_t slot = 0; slot < config_.slotCount; ++slot)
            {
                Entry &entry = Touch(slot, tick);
                entry.confirmed = true;
                entry.input = config_.layout.neutral;
            }
        }
        nextLocal_ = config_.inputDelay;
        AdvanceConfirmed();
    }

    const RollbackSession::Entry *RollbackSession::Find(const std::uint32_t slot, const std::uint32_t tick) const noexcept
    {
        const Entry &entry = slots_[slot].ring[tick % kRollbackInputWindow];
        return entry.occupied && entry.tick == tick ? &entry : nullptr;
    }

    RollbackSession::Entry &RollbackSession::Touch(const std::uint32_t slot, const std::uint32_t tick) noexcept
    {
        Entry &entry = slots_[slot].ring[tick % kRollbackInputWindow];
        if (!entry.occupied || entry.tick != tick)
        {
            entry = Entry{};
            entry.tick = tick;
            entry.occupied = true;
        }
        return entry;
    }

    N2Net::SubmitResult RollbackSession::SubmitLocal(const N2Net::LockstepInput &input, std::uint32_t &assignedTick) noexcept
    {
        if (!valid_)
        {
            return N2Net::SubmitResult::BadSlot;
        }
        if (input.size != config_.layout.payloadBytes)
        {
            return N2Net::SubmitResult::BadPayload;
        }
        if (nextLocal_ - first_ >= kRollbackInputWindow)
        {
            return N2Net::SubmitResult::TooFar;
        }
        assignedTick = nextLocal_;
        Entry &entry = Touch(config_.localSlot, nextLocal_);
        entry.confirmed = true;
        entry.input = input;
        ++nextLocal_;
        return N2Net::SubmitResult::Accepted;
    }

    N2Net::SubmitResult RollbackSession::SubmitRemote(const std::uint32_t slot, const std::uint32_t tick,
                                                      const N2Net::LockstepInput &input) noexcept
    {
        if (!valid_ || slot >= config_.slotCount || slot == config_.localSlot)
        {
            return N2Net::SubmitResult::BadSlot;
        }
        if (input.size != config_.layout.payloadBytes)
        {
            return N2Net::SubmitResult::BadPayload;
        }
        if (static_cast<std::int32_t>(tick - first_) < 0)
        {
            return N2Net::SubmitResult::TooOld;
        }
        if (tick - first_ >= kRollbackInputWindow)
        {
            return N2Net::SubmitResult::TooFar;
        }
        if (const Entry *const existing = Find(slot, tick); existing != nullptr && existing->confirmed)
        {
            return existing->input == input ? N2Net::SubmitResult::Duplicate : N2Net::SubmitResult::Conflict;
        }
        Entry &entry = Touch(slot, tick);
        entry.confirmed = true;
        entry.input = input;
        if (static_cast<std::int32_t>(tick - current_) < 0 && entry.usedValid && !(entry.used == input))
        {
            ++stats_.mispredictions;
            if (!pending_.has_value() || static_cast<std::int32_t>(tick - *pending_) < 0)
            {
                pending_ = tick;
            }
        }
        AdvanceConfirmed();
        return N2Net::SubmitResult::Accepted;
    }

    N2Net::LockstepInput RollbackSession::Predict(const std::uint32_t slot, const std::uint32_t tick) const
    {
        N2Net::LockstepInput last = config_.layout.neutral;
        for (std::uint32_t back = 1; back <= 64 && back <= tick; ++back)
        {
            const Entry *const entry = Find(slot, tick - back);
            if (entry != nullptr && entry->confirmed)
            {
                last = entry->input;
                break;
            }
        }
        if (host_.predict)
        {
            return host_.predict(slot, tick, last);
        }
        return last;
    }

    bool RollbackSession::AllConfirmed(const std::uint32_t tick) const noexcept
    {
        for (std::uint32_t slot = 0; slot < config_.slotCount; ++slot)
        {
            const Entry *const entry = Find(slot, tick);
            if (entry == nullptr || !entry->confirmed)
            {
                return false;
            }
        }
        return true;
    }

    void RollbackSession::GatherInputs(const std::uint32_t tick, N2Net::TickInputs &out)
    {
        out = N2Net::TickInputs{};
        out.tick = tick;
        for (std::uint32_t slot = 0; slot < config_.slotCount; ++slot)
        {
            const Entry *const known = Find(slot, tick);
            const N2Net::LockstepInput input = known != nullptr && known->confirmed ? known->input : Predict(slot, tick);
            out.inputs[slot] = input;
            Entry &entry = Touch(slot, tick);
            entry.used = input;
            entry.usedValid = true;
        }
    }

    void RollbackSession::AdvanceConfirmed()
    {
        const std::uint32_t before = first_;
        while (first_ != current_)
        {
            bool settled = true;
            for (std::uint32_t slot = 0; slot < config_.slotCount && settled; ++slot)
            {
                const Entry *const entry = Find(slot, first_);
                settled = entry != nullptr && entry->confirmed && entry->usedValid && entry->used == entry->input;
            }
            if (!settled)
            {
                break;
            }
            ++first_;
        }
        if (first_ != before)
        {
            events_.Confirm(first_);
            if (host_.confirmed)
            {
                host_.confirmed(first_);
            }
        }
    }

    bool RollbackSession::RunTick(const std::uint32_t tick)
    {
        N2Net::TickInputs inputs;
        GatherInputs(tick, inputs);
        events_.BeginTick(tick);
        if (!host_.step(inputs))
        {
            Fail(std::format("the simulation refused tick {}", tick));
            return false;
        }
        return true;
    }

    bool RollbackSession::Rollback(std::uint32_t &resimulated)
    {
        const std::uint32_t target = *pending_;
        pending_.reset();
        const std::size_t index = target % ring_.size();
        if (static_cast<std::int32_t>(target - first_) < 0 || !ringValid_[index] || ring_[index].tick != target)
        {
            Fail(std::format("no snapshot for tick {} (confirmed tick {}, current tick {})", target, first_, current_));
            return false;
        }

        events_.BeginResimulation(target);
        if (const auto restored = host_.restore(ring_[index]); !restored.has_value())
        {
            Fail("restoring tick " + std::to_string(target) + " failed: " + restored.error());
            return false;
        }
        const std::uint32_t endTick = current_;
        for (std::uint32_t tick = target; tick != endTick; ++tick)
        {
            if (tick != target)
            {
                const std::size_t slotIndex = tick % ring_.size();
                if (!host_.save(tick, ring_[slotIndex]))
                {
                    Fail(std::format("saving the state before tick {} failed", tick));
                    return false;
                }
                ringValid_[slotIndex] = true;
                ++stats_.snapshotsSaved;
            }
            if (!RunTick(tick))
            {
                return false;
            }
            ++resimulated;
        }
        events_.EndResimulation();
        if (host_.resimulated)
        {
            host_.resimulated();
        }
        ++stats_.rollbacks;
        stats_.ticksResimulated += resimulated;
        stats_.deepestRollback = std::max(stats_.deepestRollback, resimulated);
        AdvanceConfirmed();
        return true;
    }

    AdvanceReport RollbackSession::Advance()
    {
        AdvanceReport report;
        if (failed_)
        {
            report.result = AdvanceResult::Failed;
            return report;
        }
        if (pending_.has_value())
        {
            report.rolledBack = true;
            if (!Rollback(report.resimulated))
            {
                report.result = AdvanceResult::Failed;
                return report;
            }
        }

        const std::uint32_t tick = current_;
        const Entry *const local = Find(config_.localSlot, tick);
        if (local == nullptr || !local->confirmed)
        {
            report.result = AdvanceResult::WaitingForInput;
            return report;
        }
        const bool everyoneKnown = AllConfirmed(tick);
        if (config_.rollbackWindow == 0)
        {
            if (!everyoneKnown)
            {
                report.result = AdvanceResult::WaitingForInput;
                return report;
            }
        }
        else if (tick - first_ >= config_.rollbackWindow)
        {
            ++stats_.windowFullWaits;
            report.result = AdvanceResult::WindowFull;
            return report;
        }

        // A tick on fully confirmed input right behind the confirmed tick can never be rolled back to: no snapshot
        if (config_.rollbackWindow > 0 && !(everyoneKnown && tick == first_))
        {
            const std::size_t index = tick % ring_.size();
            if (!host_.save(tick, ring_[index]))
            {
                Fail(std::format("saving the state before tick {} failed", tick));
                report.result = AdvanceResult::Failed;
                return report;
            }
            ringValid_[index] = true;
            ++stats_.snapshotsSaved;
        }
        if (!everyoneKnown)
        {
            ++stats_.ticksPredicted;
        }
        if (!RunTick(tick))
        {
            report.result = AdvanceResult::Failed;
            return report;
        }
        ++current_;
        ++stats_.ticksRun;
        AdvanceConfirmed();
        report.result = AdvanceResult::Ran;
        return report;
    }

    const WorldSnapshot *RollbackSession::SnapshotAt(const std::uint32_t tick) const noexcept
    {
        if (ring_.empty())
        {
            return nullptr;
        }
        const std::size_t index = tick % ring_.size();
        return ringValid_[index] && ring_[index].tick == tick ? &ring_[index] : nullptr;
    }

    std::optional<N2Net::LockstepInput> RollbackSession::ConfirmedInput(const std::uint32_t slot,
                                                                        const std::uint32_t tick) const noexcept
    {
        if (slot >= config_.slotCount)
        {
            return std::nullopt;
        }
        const Entry *const entry = Find(slot, tick);
        if (entry == nullptr || !entry->confirmed)
        {
            return std::nullopt;
        }
        return entry->input;
    }

    std::uint32_t RollbackSession::NextNeeded(const std::uint32_t slot) const noexcept
    {
        std::uint32_t tick = first_;
        while (tick - first_ < kRollbackInputWindow)
        {
            const Entry *const entry = Find(slot, tick);
            if (entry == nullptr || !entry->confirmed)
            {
                break;
            }
            ++tick;
        }
        return tick;
    }

    void RollbackSession::Fail(std::string text)
    {
        failed_ = true;
        error_ = std::move(text);
    }
}
