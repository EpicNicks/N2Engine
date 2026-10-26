#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "engine/network/RollbackEvents.hpp"
#include "engine/network/RollbackWorld.hpp"
#include "engine/network/n2net/Lockstep.hpp"

namespace N2Engine::Network
{
    /// The most ticks a session may predict ahead of the newest tick whose inputs are all confirmed (design 4.2: T 0-15)
    inline constexpr std::uint32_t kMaxRollbackWindow = 15;
    /// Ticks of input kept per slot (a power of two, well above window + input delay)
    inline constexpr std::uint32_t kRollbackInputWindow = 128;

    struct RollbackConfig
    {
        std::uint32_t slotCount = 2;
        std::uint32_t localSlot = 0;
        /// The input sampled while the simulation is at tick T is for tick T + inputDelay (as in lockstep)
        std::uint32_t inputDelay = 2;
        /// 0 is pure lockstep (a tick runs only when every input is confirmed, nothing is predicted, no snapshots are
        /// taken); otherwise the most ticks that may run on predicted input at once, which is also the most a rollback
        /// resimulates
        std::uint32_t rollbackWindow = 8;
        /// When effects emitted through Events() play: at once and cancelled on a rollback, or only once confirmed
        RollbackEventPolicy eventPolicy = RollbackEventPolicy::Immediate;
        /// The payload size and neutral input every peer agreed on (the same LockstepLayoutInfo lockstep uses)
        N2Net::LockstepLayoutInfo layout;
    };

    /// What the session needs from the simulation it drives
    struct RollbackHost
    {
        /// Writes the state before tick `tick` into `out`
        std::function<bool(std::uint32_t tick, WorldSnapshot &out)> save;
        /// Puts a snapshot back
        std::function<std::expected<void, std::string>(const WorldSnapshot &)> restore;
        /// Runs one tick with these inputs (confirmed or predicted; inputs.tick is the tick). False ends the session.
        /// Effects go through RollbackSession::Events, spawns and despawns through RollbackWorld.
        std::function<bool(const N2Net::TickInputs &)> step;
        /// Optional: every tick before `firstUnconfirmed` is final and no snapshot at an earlier tick is needed any more
        /// (RollbackWorld::ReleaseConfirmed)
        std::function<void(std::uint32_t firstUnconfirmed)> confirmed;
        /// Optional: a resimulation caught up (RollbackWorld::ReleasePool)
        std::function<void()> resimulated;
        /// Optional: the guess for `slot`'s input at `tick`, given the last confirmed input of the slot before it (the
        /// slot's neutral input when there is none). The default repeats it (design 4.2). It must be a pure function of
        /// its arguments.
        std::function<N2Net::LockstepInput(std::uint32_t slot, std::uint32_t tick, const N2Net::LockstepInput &last)> predict;
    };

    enum class AdvanceResult : std::uint8_t
    {
        /// One new tick ran
        Ran,
        /// The local input for the next tick was not submitted yet (a lockstep session would wait for a remote one; a
        /// rollback session predicts it)
        WaitingForInput,
        /// rollbackWindow ticks are already running on predicted input: the session waits for confirmations
        WindowFull,
        /// The session ended in an error (see RollbackSession::Error); nothing runs any more
        Failed,
    };

    struct AdvanceReport
    {
        AdvanceResult result = AdvanceResult::WaitingForInput;
        /// A rollback was performed first
        bool rolledBack = false;
        /// Ticks the rollback resimulated
        std::uint32_t resimulated = 0;
    };

    struct RollbackStats
    {
        std::uint64_t ticksRun = 0;
        std::uint64_t ticksPredicted = 0;
        std::uint64_t rollbacks = 0;
        std::uint64_t ticksResimulated = 0;
        /// Remote inputs that differed from the guess the simulation had used
        std::uint64_t mispredictions = 0;
        std::uint32_t deepestRollback = 0;
        std::uint64_t windowFullWaits = 0;
        std::uint64_t snapshotsSaved = 0;
    };

    /**
     * Rollback netcode (C5, design 4.1-4.2): the input side and the state machine of a rollback session, with no
     * transport and no clock. The host feeds it inputs (SubmitLocal for its own, SubmitRemote for the others', from
     * InputBundles or anything else) and calls Advance for each tick that is due; the session runs ticks on confirmed
     * input where it has it and on a guess (the slot's last confirmed input) where it does not, as long as no more than
     * rollbackWindow ticks are unconfirmed.
     *
     * A snapshot of the state BEFORE each unconfirmed tick is kept in a ring of rollbackWindow + 2 (the host's save). When
     * a remote input arrives for a tick that already ran on a guess and differs from it (a misprediction), the next
     * Advance restores the snapshot at the oldest such tick and resimulates up to the present, with every input now known
     * and a fresh guess for the rest, then runs the new tick. A late input that equals the guess changes nothing. The
     * result after all inputs are in is the state a lockstep session fed the same inputs reaches.
     *
     * Determinism requirements are lockstep's (docs/netcode.html): step must be a pure function of the state and the
     * inputs, and save/restore must capture all of it (RollbackWorld). With rollbackWindow 0 the session is lockstep: it
     * never predicts and never saves.
     *
     * The ticks are plain 32-bit counters from 0; a session does not run across 2^32 ticks (the lockstep scheduler's
     * wrap handling is not duplicated here). Single-threaded.
     */
    class RollbackSession
    {
    public:
        RollbackSession(const RollbackConfig &config, RollbackHost host);

        [[nodiscard]] bool Valid() const noexcept { return valid_; }
        [[nodiscard]] const RollbackConfig &Config() const noexcept { return config_; }

        /// The local slot's input for its next tick (CurrentTick + inputDelay when called once per tick); the tick it was
        /// assigned is in `assignedTick`. TooFar when the slot is kRollbackInputWindow ticks ahead of the confirmed tick.
        N2Net::SubmitResult SubmitLocal(const N2Net::LockstepInput &input, std::uint32_t &assignedTick) noexcept;

        /// A remote slot's input for `tick`. Accepted for a tick not yet run (it is used instead of a guess) and for a
        /// tick that ran on a guess (a differing input schedules a rollback for the next Advance). Duplicate for the same
        /// input again, Conflict for a different one (the first stays). TooOld before the confirmed tick, TooFar a
        /// window past it, BadSlot for the local slot or a slot out of range, BadPayload for a wrong length.
        N2Net::SubmitResult SubmitRemote(std::uint32_t slot, std::uint32_t tick, const N2Net::LockstepInput &input) noexcept;

        /// Performs a pending rollback, then runs one new tick if it may. Call it in a loop for the ticks that are due.
        AdvanceReport Advance();

        /// The next tick to run (the number of ticks run)
        [[nodiscard]] std::uint32_t CurrentTick() const noexcept { return current_; }
        /// Every tick before it ran on confirmed input only and is final
        [[nodiscard]] std::uint32_t ConfirmedTick() const noexcept { return first_; }
        /// The tick a rollback is scheduled for, if any
        [[nodiscard]] std::optional<std::uint32_t> PendingRollback() const noexcept { return pending_; }
        /// The snapshot of the state before `tick`, if the ring still holds it (ConfirmedTick to CurrentTick)
        [[nodiscard]] const WorldSnapshot *SnapshotAt(std::uint32_t tick) const noexcept;
        /// The confirmed input of (slot, tick) while it is held
        [[nodiscard]] std::optional<N2Net::LockstepInput> ConfirmedInput(std::uint32_t slot, std::uint32_t tick) const noexcept;
        /// First tick of `slot` whose confirmed input is not known (every earlier one from ConfirmedTick on is), which is
        /// what an InputBundle acknowledgement reports back to the other peers
        [[nodiscard]] std::uint32_t NextNeeded(std::uint32_t slot) const noexcept;

        [[nodiscard]] RollbackEventQueue &Events() noexcept { return events_; }
        [[nodiscard]] const RollbackStats &Stats() const noexcept { return stats_; }
        [[nodiscard]] const std::string &Error() const noexcept { return error_; }

    private:
        struct Entry
        {
            std::uint32_t tick = 0;
            bool occupied = false;
            bool confirmed = false;
            bool usedValid = false;
            N2Net::LockstepInput input;
            N2Net::LockstepInput used;
        };

        struct Slot
        {
            std::array<Entry, kRollbackInputWindow> ring{};
        };

        [[nodiscard]] const Entry *Find(std::uint32_t slot, std::uint32_t tick) const noexcept;
        Entry &Touch(std::uint32_t slot, std::uint32_t tick) noexcept;
        [[nodiscard]] N2Net::LockstepInput Predict(std::uint32_t slot, std::uint32_t tick) const;
        void GatherInputs(std::uint32_t tick, N2Net::TickInputs &out);
        [[nodiscard]] bool AllConfirmed(std::uint32_t tick) const noexcept;
        void AdvanceConfirmed();
        bool RunTick(std::uint32_t tick);
        bool Rollback(std::uint32_t &resimulated);
        void Fail(std::string text);

        bool valid_ = false;
        RollbackConfig config_;
        RollbackHost host_;
        std::array<Slot, N2Net::kMaxLockstepSlots> slots_{};
        std::vector<WorldSnapshot> ring_;
        std::vector<bool> ringValid_;
        std::uint32_t current_ = 0;
        std::uint32_t first_ = 0;
        std::uint32_t nextLocal_ = 0;
        std::optional<std::uint32_t> pending_;
        bool failed_ = false;
        std::string error_;
        RollbackEventQueue events_;
        RollbackStats stats_;
    };
}
