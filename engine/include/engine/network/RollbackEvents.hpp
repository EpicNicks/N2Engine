#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace N2Engine::Network
{
    /// Identifies a simulation-triggered effect (a sound, a particle burst): the tick it happened in, the net object it
    /// belongs to (0: none), the game's own event id, and which of several identical emissions in that tick it was. A
    /// resimulation that triggers the same effect produces the same key, which is how it is recognised as the same
    /// effect and not played twice.
    struct RollbackEventKey
    {
        std::uint32_t tick = 0;
        std::uint32_t netId = 0;
        std::uint32_t eventId = 0;
        std::uint32_t occurrence = 0;

        friend bool operator==(const RollbackEventKey &, const RollbackEventKey &) = default;
        friend std::strong_ordering operator<=>(const RollbackEventKey &a, const RollbackEventKey &b)
        {
            return std::tie(a.tick, a.netId, a.eventId, a.occurrence) <=> std::tie(b.tick, b.netId, b.eventId, b.occurrence);
        }
    };

    struct RollbackEvent
    {
        RollbackEventKey key;
        std::vector<std::uint8_t> payload;
    };

    enum class RollbackEventPolicy : std::uint8_t
    {
        /// Played the moment the simulation emits it, even though its tick may still be rolled back; cancelled (the
        /// cancel handler runs) if a rollback removes it. Lowest latency, and the effect must be cancellable.
        Immediate,
        /// Played only when its tick is confirmed (every input of the tick known and the tick will never be redone).
        /// Never cancelled, a few ticks late. The safe choice for effects that cannot be taken back.
        OnConfirm,
    };

    struct RollbackEventStats
    {
        std::uint64_t emitted = 0;
        std::uint64_t played = 0;
        /// Re-emitted by a resimulation with the same key and payload: not played again
        std::uint64_t matched = 0;
        std::uint64_t cancelled = 0;
        /// Dropped by a resimulation before OnConfirm ever played them
        std::uint64_t dropped = 0;
    };

    /**
     * Rollback-aware events (design 4.4, point 4). The simulation calls Emit for each effect; the queue decides when the
     * game sees it. Under Immediate an event is played once when first emitted. A rollback to tick R makes the following
     * resimulation (BeginResimulation(R) ... EndResimulation) re-emit the events of ticks >= R: one with the same key and
     * payload as before is recognised and not played again; one the resimulation does not produce is cancelled; a new one
     * is played. Confirm(t) finalises everything before tick t: such events can no longer be cancelled and leave the log.
     * Under OnConfirm nothing is played until Confirm.
     *
     * An event whose payload changed under the same key counts as a different event: the old one is cancelled, the new
     * one played. The log is bounded by the rollback window: an event stays only from its tick until it is confirmed.
     * Single-threaded; the handlers run inside Emit, EndResimulation or Confirm and may not call back into the queue.
     */
    class RollbackEventQueue
    {
    public:
        using Handler = std::function<void(const RollbackEvent &)>;

        explicit RollbackEventQueue(RollbackEventPolicy policy = RollbackEventPolicy::Immediate) : _policy(policy) {}

        void SetPlayHandler(Handler handler) { _play = std::move(handler); }
        void SetCancelHandler(Handler handler) { _cancel = std::move(handler); }
        [[nodiscard]] RollbackEventPolicy Policy() const noexcept { return _policy; }

        /// The simulation is about to run tick `tick` (the first run or a resimulation): the occurrence counters restart
        void BeginTick(std::uint32_t tick);

        /// The simulation triggers an effect during the current tick. Returns the key it was given.
        RollbackEventKey Emit(std::uint32_t netId, std::uint32_t eventId, std::span<const std::uint8_t> payload = {});

        /// A rollback to `fromTick` is about to resimulate: the events of ticks >= fromTick are unconfirmed again
        void BeginResimulation(std::uint32_t fromTick);
        /// The resimulation caught up: events from `fromTick` on that were not re-emitted are cancelled (or dropped)
        void EndResimulation();

        /// Every event before tick `firstUnconfirmed` is final (OnConfirm: played now, in key order)
        void Confirm(std::uint32_t firstUnconfirmed);

        /// Events in the log (played or waiting, not yet confirmed)
        [[nodiscard]] std::size_t Pending() const noexcept { return _log.size(); }
        [[nodiscard]] const RollbackEventStats &Stats() const noexcept { return _stats; }

    private:
        struct Entry
        {
            RollbackEvent event;
            bool played = false;
            bool seen = true;
        };

        RollbackEventPolicy _policy;
        Handler _play;
        Handler _cancel;
        std::uint32_t _tick = 0;
        bool _resimulating = false;
        std::uint32_t _resimFrom = 0;
        std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> _occurrences;
        std::map<RollbackEventKey, Entry> _log;
        RollbackEventStats _stats;
    };
}
