#pragma once
#include <chrono>
#include <cstdint>

namespace N2Engine
{
    class Application;
    class SimulationClock;

    class Time
    {
        friend class Application;
        friend class SimulationClock;

        using TimePoint = std::chrono::high_resolution_clock::time_point;

    private:
        static double scaledDeltaTime;
        static double unscaledDeltaTime;

        static double time;
        static double unscaledTime;

        static float timeScale;

        static double fixedDeltaTime;
        static double fixedUnscaledDeltaTime;
        /// Network simulation modes: the fixed delta is the unscaled one whatever the time scale
        static bool fixedIgnoresTimeScale;
        /// Fixed ticks begun (see SimulationClock)
        static std::uint64_t tick;

        static TimePoint lastFrameTime;

        static void Init();
        static void Update();
        /// A frame of exactly `seconds` instead of the clock's (Application::Tick with a delta: the editor's Step); the
        /// clock itself is left alone
        static void Advance(double seconds);
        /// Makes GetFixedDeltaTime() follow (false, the default) or ignore (true) the time scale
        static void SetFixedIgnoresTimeScale(bool ignores);
        static double ScaledFixedDelta();
        /// Makes the next Update measure from now (after a pause, so the pause isn't one long frame)
        static void ResetFrameClock();

    public:
        static float GetDeltaTime();
        static float GetFixedDeltaTime();
        static float GetUnscaledDeltaTime();
        static float GetTime();
        static float GetUnscaledTime();
        static float GetFixedUnscaledDeltaTime();
        /// The number of fixed ticks begun: 0 before the first, incremented once at the start of each (so inside
        /// tick N's FixedUpdate it is N). Never wraps in practice. See SimulationClock.
        static std::uint64_t GetTick();

        static void SetTimeScale(float scale);
        static float GetTimeScale();

        /// The unscaled fixed timestep, in seconds (0.02 by default, and again after Application::Init); the
        /// scaled one follows the time scale. False, changing nothing, unless seconds is finite and positive.
        static bool SetFixedTimestep(double seconds);
        /// The unscaled fixed timestep, in seconds, at full precision (what SetFixedTimestep set)
        static double GetFixedTimestep();
    };
}