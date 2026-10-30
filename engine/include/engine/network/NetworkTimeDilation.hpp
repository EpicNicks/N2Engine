#pragma once

#include <cstdint>

#include "engine/network/NetworkSettings.hpp"

namespace N2Engine::Network
{
    /**
     * The knobs of client time dilation (netcode stage B6 follow-up, opt-in). An authoritative client should stamp its
     * input so that it reaches the server just before the server needs it (NetworkClient::SuggestedInputTick). The tick
     * the game actually stamps drifts away from that target when the latency changes; dilation closes the gap by running
     * the client's fixed-step clock a little faster or slower (scale in [minScale, maxScale], 1 = real time) instead of
     * jumping, and resyncs by holding or skipping whole ticks when the gap is far too large for a gentle correction.
     *
     * Off by default (enabled = false): then nothing here runs and the client's clock is exactly what it was. It only ever
     * changes how fast the CLIENT's fixed ticks come; the physics dt stays one fixed tick, the server is never touched,
     * and lockstep and rollback sessions (which do not use NetworkClient) are not affected.
     */
    struct TimeDilationOptions
    {
        bool enabled = false;
        /// The fixed-step scale stays within these (clamped to 0.5..1 and 1..2). 0.9..1.1 is imperceptible in play.
        double minScale = 0.9;
        double maxScale = 1.1;
        /// The smoothed lead may be this many ticks off the target without any correction (also the hysteresis: a
        /// correction that has started goes on until the error is under half of it)
        double deadZoneTicks = 0.5;
        /// Scale change per tick of error beyond the dead zone: 0.02 means a 5 tick error asks for the full 10 percent
        double gainPerTick = 0.02;
        /// The lead is low-pass filtered with this time constant before it is judged
        std::uint64_t smoothingMicros = 250'000;
        /// The scale moves at most this much per second (so it never steps)
        double maxSlewPerSecond = 0.25;
        /// A smoothed error beyond this many ticks is not corrected gently: the clock holds (client ahead) or runs extra
        /// ticks (client behind) that many ticks at once, once, and the controller waits resyncCooldownMicros before it
        /// judges again
        double resyncTicks = 10.0;
        std::uint64_t resyncCooldownMicros = 1'000'000;

        /// Every field into its range (a non-finite value becomes the default)
        [[nodiscard]] TimeDilationOptions Clamped() const noexcept;
        /// enabled, minScale and maxScale from the project's network.authoritative.timeDilation block; the rest default
        [[nodiscard]] static TimeDilationOptions FromSettings(const NetworkSettings &settings) noexcept;
    };

    struct TimeDilationStats
    {
        std::uint64_t updates = 0;
        /// Updates whose requested scale was cut by minScale or maxScale
        std::uint64_t clampedUpdates = 0;
        std::uint64_t resyncs = 0;
        /// Ticks the clock was told to hold (client ahead) and to run extra (client behind), in all resyncs
        std::uint64_t ticksHeld = 0;
        std::uint64_t ticksSkipped = 0;
        /// The largest raw error seen, in ticks (absolute)
        double maxAbsError = 0.0;
    };

    /**
     * Turns the lead error into a fixed-step time scale. Pure: no clock of its own (the caller passes the time), no
     * engine objects, deterministic.
     *
     * error = (the tick the client is stamping now) - (the tick it should be stamping), in ticks; positive means the
     * client is too far ahead, so it must slow down. Update() low-pass filters it, applies a dead zone with hysteresis,
     * asks for scale = 1 - gainPerTick * (error beyond the dead zone), clamps that to [minScale, maxScale] and lets the
     * scale approach it at most maxSlewPerSecond. Beyond resyncTicks it asks for a one-off whole-tick jump instead
     * (TakeResyncTicks) and holds the scale at 1 for the cooldown.
     */
    class TimeDilationController
    {
    public:
        explicit TimeDilationController(const TimeDilationOptions &options = {}) noexcept : _options(options.Clamped()) {}

        /// Changes the options (clamped). Turning dilation on or off, or changing it while off, starts over.
        void SetOptions(const TimeDilationOptions &options) noexcept;
        [[nodiscard]] const TimeDilationOptions &Options() const noexcept { return _options; }

        /// Back to scale 1 with nothing remembered (statistics kept)
        void Reset() noexcept;

        /// One measurement at `nowMicros` (monotonic) of `errorTicks`. A non-finite error is ignored. Disabled: scale 1.
        void Update(std::uint64_t nowMicros, double errorTicks) noexcept;

        /// The scale for the fixed-step clock now: 1 while disabled
        [[nodiscard]] double Scale() const noexcept { return _options.enabled ? _scale : 1.0; }
        [[nodiscard]] double SmoothedError() const noexcept { return _filter; }
        /// True while a correction is in progress (the smoothed error left the dead zone and has not come back)
        [[nodiscard]] bool Correcting() const noexcept { return _correcting; }
        [[nodiscard]] bool InCooldown(std::uint64_t nowMicros) const noexcept
        {
            return _options.enabled && nowMicros < _cooldownUntil;
        }

        /// The whole ticks the fixed clock should advance by now, once: positive runs that many extra ticks, negative
        /// holds that many. 0 when no resync is pending. Clears it.
        [[nodiscard]] std::int32_t TakeResyncTicks() noexcept;

        [[nodiscard]] const TimeDilationStats &Stats() const noexcept { return _stats; }

    private:
        void MoveScale(double target, double seconds) noexcept;

        TimeDilationOptions _options;
        TimeDilationStats _stats;
        double _scale = 1.0;
        double _filter = 0.0;
        bool _seeded = false;
        bool _correcting = false;
        std::uint64_t _last = 0;
        std::uint64_t _cooldownUntil = 0;
        std::int32_t _pendingResync = 0;
    };
}
