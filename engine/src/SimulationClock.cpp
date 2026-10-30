#include "engine/SimulationClock.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "engine/Logger.hpp"
#include "engine/Time.hpp"

using namespace N2Engine;

const char *N2Engine::ToString(const SimulationMode mode)
{
    switch (mode)
    {
    case SimulationMode::None:
        return "none";
    case SimulationMode::Authoritative:
        return "authoritative";
    case SimulationMode::Deterministic:
        return "deterministic";
    }
    return "none";
}

void SimulationClock::SetMode(const SimulationMode mode)
{
    _mode = mode;
    _behind = false;
    if (mode == SimulationMode::None)
    {
        // The None loop has no catch-up limit and holds at most MaxFrameTime: a debt a network mode was carrying must
        // not become a burst of ticks
        _accumulator = std::min(_accumulator, MaxFrameTime);
    }
}

std::uint32_t SimulationClock::GetTicksOwed(const double step) const
{
    if (!(step > 0.0))
    {
        return 0;
    }
    const double owed = std::floor(_accumulator / step);
    constexpr double Max = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(std::clamp(owed, 0.0, Max));
}

std::uint64_t SimulationClock::GetTick() const
{
    return Time::GetTick();
}

void SimulationClock::ResetAccumulator()
{
    _accumulator = 0.0;
    _ticksThisFrame = 0;
    _behind = false;
}

void SimulationClock::Reset()
{
    ResetAccumulator();
    Time::tick = 0;
}

void SimulationClock::BeginFrame(const double unscaledDelta)
{
    _ticksThisFrame = 0;
    // None: the loop as it has always been (a long stall is clamped, the rest dropped). Network modes keep all of it.
    if (IsNetworked())
    {
        // A NaN, infinite or negative delta would poison the accumulator (every comparison against NaN is false, so
        // the loop would never see itself behind): it is not time, so none is added
        if (std::isfinite(unscaledDelta) && unscaledDelta > 0.0)
        {
            _accumulator += unscaledDelta;
        }
        return;
    }
    _accumulator += std::min(unscaledDelta, MaxFrameTime);
}

void SimulationClock::AddOwedSeconds(const double seconds)
{
    if (!IsNetworked() || !std::isfinite(seconds))
    {
        return;
    }
    _accumulator = std::max(_accumulator + seconds, -MaxOwedSeconds);
}

bool SimulationClock::TickDue(const double step) const
{
    if (_accumulator < step)
    {
        return false;
    }
    return !IsNetworked() || _maxCatchUpTicks == 0 || _ticksThisFrame < _maxCatchUpTicks;
}

void SimulationClock::CompleteTick(const double step)
{
    _accumulator -= step;
    ++_ticksThisFrame;
}

void SimulationClock::EndFrame(const double step)
{
    _behind = IsNetworked() && step > 0.0 && _accumulator >= step;
    if (!_behind)
    {
        return;
    }
    const bool beyondLimit = _accumulator > MaxOwedSeconds;
    if (_behindHandler)
    {
        SimulationBehind report;
        report.tick = Time::GetTick();
        report.ticksOwed = GetTicksOwed(step);
        report.ticksRunThisFrame = _ticksThisFrame;
        report.owedSeconds = _accumulator;
        report.beyondLimit = beyondLimit;
        _behindHandler(report);
        return;
    }
    if (beyondLimit)
    {
        // Nobody is resyncing (no netcode runs yet, or it did not install a handler): a clock this far behind would
        // otherwise run catch-up ticks for ever, so it drops the debt and says so
        Logger::Warn(std::format("The {} simulation loop ended a frame {:.2f} s behind (more than {:.2f} s); dropping "
                                 "the owed time to resync. Install a SimulationClock behind handler to resync "
                                 "differently.",
                                 ToString(_mode), _accumulator, MaxOwedSeconds));
        ResetAccumulator();
    }
}

void SimulationClock::BeginTick()
{
    ++Time::tick;
}

void SimulationClock::SetTick(const std::uint64_t tick)
{
    Time::tick = tick;
}
