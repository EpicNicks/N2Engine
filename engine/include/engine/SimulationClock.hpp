#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace N2Engine
{
    /// How the application's fixed-step loop is driven. None is the offline loop every project has always had; the
    /// two network modes (the Server authoritative and Deterministic presets) are the tick-owned loop of the netcode.
    /// A host or a test sets it through Application::SetSimulationMode; the network presets will drive it later.
    enum class SimulationMode
    {
        None,
        Authoritative,
        Deterministic,
    };

    /// "none", "authoritative" or "deterministic"
    [[nodiscard]] const char *ToString(SimulationMode mode);

    /// What a network-mode frame reports when it ends with whole ticks still owed (it ran as many as the catch-up
    /// limit allows and time is left): nothing of that time has been dropped
    struct SimulationBehind
    {
        /// The tick counter now (Time::GetTick())
        std::uint64_t tick = 0;
        /// Whole fixed ticks still owed
        std::uint32_t ticksOwed = 0;
        /// Ticks this frame did run
        std::uint32_t ticksRunThisFrame = 0;
        /// The time still owed, in seconds (not a whole number of ticks)
        double owedSeconds = 0.0;
    };

    /**
     * Decides how many fixed ticks run in a frame and owns the tick counter (Time::GetTick()).
     *
     * The frame loop feeds it the frame's real delta (BeginFrame), then asks "is a tick due?" and reports each tick it
     * ran (CompleteTick) until none is; the clock itself never calls into the scene.
     *
     * None mode is the loop as it has always been: the frame delta is clamped at MaxFrameTime and the rest dropped,
     * and the number of ticks is not limited. Network modes drop nothing: the whole delta is added, at most
     * GetMaxCatchUpTicks() ticks run per frame, and what is still owed stays in the accumulator and is reported
     * through the behind handler (and IsBehind) so the netcode can resync instead of the loop silently losing time.
     * Network modes also make Time::GetFixedDeltaTime() independent of Time::GetTimeScale().
     *
     * The counter: 0 before the first tick, incremented once per tick at its start (BeginTick), so inside tick N's
     * FixedUpdate Time::GetTick() is N and between frames it is the number of ticks run. It is a 64-bit count and does
     * not wrap in practice (at 1000 ticks per second it would take 584 million years).
     */
    class SimulationClock
    {
    public:
        /// None mode: the longest frame the accumulator takes in (seconds); a stall beyond it is dropped
        static constexpr double MaxFrameTime = 0.25;
        /// Network modes: ticks per frame before the rest is left owed and reported
        static constexpr std::uint32_t DefaultMaxCatchUpTicks = 30;

        using BehindHandler = std::function<void(const SimulationBehind &)>;

        /// Switches the loop's mode. The tick counter is kept, and so is the accumulator, except that going to None
        /// keeps at most MaxFrameTime of it. It does not touch Time: Application::SetSimulationMode also makes
        /// Time::GetFixedDeltaTime() follow (None) or ignore (network modes) the time scale.
        void SetMode(SimulationMode mode);
        [[nodiscard]] SimulationMode GetMode() const { return _mode; }
        [[nodiscard]] bool IsNetworked() const { return _mode != SimulationMode::None; }

        /// Network modes: the most ticks one frame may run (0 means no limit). Whole ticks beyond it stay owed.
        void SetMaxCatchUpTicks(std::uint32_t ticks) { _maxCatchUpTicks = ticks; }
        [[nodiscard]] std::uint32_t GetMaxCatchUpTicks() const { return _maxCatchUpTicks; }

        /// Called at the end of a network-mode frame that leaves whole ticks owed; an empty function removes it
        void SetBehindHandler(BehindHandler handler) { _behindHandler = std::move(handler); }

        /// Time added but not yet run as a tick, in seconds (always under one step in None mode after a frame)
        [[nodiscard]] double GetAccumulator() const { return _accumulator; }
        /// Whether the last frame ended with whole ticks still owed (network modes only)
        [[nodiscard]] bool IsBehind() const { return _behind; }
        /// Whole ticks owed for a step of `step` seconds
        [[nodiscard]] std::uint32_t GetTicksOwed(double step) const;
        /// The tick counter (Time::GetTick())
        [[nodiscard]] std::uint64_t GetTick() const;

        /// Forgets the carried time and the behind state: the resync after a report, or a clean start. The tick
        /// counter is kept.
        void ResetAccumulator();
        /// Forgets the carried time and sets the tick counter back to 0
        void Reset();

        /// Starts a frame of `unscaledDelta` seconds of real time (None mode keeps at most MaxFrameTime of it)
        void BeginFrame(double unscaledDelta);
        /// Whether a tick of `step` seconds is due: the accumulator holds one and, in network modes, the frame has not
        /// run the catch-up limit yet
        [[nodiscard]] bool TickDue(double step) const;
        /// Takes one tick of `step` seconds out of the accumulator
        void CompleteTick(double step);
        /// Ends a frame whose ticks are done: a network-mode frame that left whole ticks owed sets IsBehind and calls
        /// the behind handler; any other clears it
        void EndFrame(double step);

        /// Starts a tick: increments the counter
        void BeginTick();

    private:
        SimulationMode _mode = SimulationMode::None;
        std::uint32_t _maxCatchUpTicks = DefaultMaxCatchUpTicks;
        double _accumulator = 0.0;
        std::uint32_t _ticksThisFrame = 0;
        bool _behind = false;
        BehindHandler _behindHandler;
    };
}
