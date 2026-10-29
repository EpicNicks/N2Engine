#include "engine/network/NetworkTimeDilation.hpp"

#include <algorithm>
#include <cmath>

namespace N2Engine::Network
{
    namespace
    {
        double Pick(const double value, const double low, const double high, const double fallback) noexcept
        {
            return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
        }
    }

    TimeDilationOptions TimeDilationOptions::Clamped() const noexcept
    {
        const TimeDilationOptions defaults;
        TimeDilationOptions out = *this;
        out.minScale = Pick(minScale, 0.5, 1.0, defaults.minScale);
        out.maxScale = Pick(maxScale, 1.0, 2.0, defaults.maxScale);
        out.deadZoneTicks = Pick(deadZoneTicks, 0.0, 10.0, defaults.deadZoneTicks);
        out.gainPerTick = Pick(gainPerTick, 0.001, 0.5, defaults.gainPerTick);
        out.maxSlewPerSecond = Pick(maxSlewPerSecond, 0.01, 10.0, defaults.maxSlewPerSecond);
        // A resync below the dead zone would never leave room for the gentle correction
        out.resyncTicks = std::max(Pick(resyncTicks, 2.0, 64.0, defaults.resyncTicks), out.deadZoneTicks + 1.0);
        out.smoothingMicros = std::clamp<std::uint64_t>(smoothingMicros, 10'000, 5'000'000);
        out.resyncCooldownMicros = std::min<std::uint64_t>(resyncCooldownMicros, 10'000'000);
        return out;
    }

    TimeDilationOptions TimeDilationOptions::FromSettings(const NetworkSettings &settings) noexcept
    {
        TimeDilationOptions out;
        out.enabled = settings.authoritative.timeDilation.enabled;
        out.minScale = settings.authoritative.timeDilation.minScale;
        out.maxScale = settings.authoritative.timeDilation.maxScale;
        return out.Clamped();
    }

    void TimeDilationController::SetOptions(const TimeDilationOptions &options) noexcept
    {
        const TimeDilationOptions next = options.Clamped();
        if (next.enabled != _options.enabled || !next.enabled)
        {
            Reset();
        }
        _options = next;
        _scale = std::clamp(_scale, _options.minScale, _options.maxScale);
    }

    void TimeDilationController::Reset() noexcept
    {
        _scale = 1.0;
        _filter = 0.0;
        _seeded = false;
        _correcting = false;
        _last = 0;
        _cooldownUntil = 0;
        _pendingResync = 0;
    }

    std::int32_t TimeDilationController::TakeResyncTicks() noexcept
    {
        const std::int32_t ticks = _pendingResync;
        _pendingResync = 0;
        return ticks;
    }

    void TimeDilationController::MoveScale(const double target, const double seconds) noexcept
    {
        const double step = _options.maxSlewPerSecond * seconds;
        _scale += std::clamp(target - _scale, -step, step);
        _scale = std::clamp(_scale, _options.minScale, _options.maxScale);
    }

    void TimeDilationController::Update(const std::uint64_t nowMicros, const double errorTicks) noexcept
    {
        if (!_options.enabled)
        {
            _scale = 1.0;
            _seeded = false;
            _correcting = false;
            return;
        }
        if (!std::isfinite(errorTicks))
        {
            return;
        }
        ++_stats.updates;
        _stats.maxAbsError = std::max(_stats.maxAbsError, std::abs(errorTicks));

        double seconds = 0.0;
        if (_seeded && nowMicros > _last)
        {
            seconds = std::min(static_cast<double>(nowMicros - _last) / 1.0e6, 1.0);
        }
        _last = nowMicros;

        if (nowMicros < _cooldownUntil)
        {
            // A resync just moved the clock: what is measured meanwhile still includes the old error. Wait, at scale 1.
            MoveScale(1.0, seconds);
            _seeded = false;
            return;
        }
        if (!_seeded)
        {
            _filter = errorTicks;
            _seeded = true;
        }
        else
        {
            const double alpha = 1.0 - std::exp(-seconds * 1.0e6 / static_cast<double>(_options.smoothingMicros));
            _filter += alpha * (errorTicks - _filter);
        }

        if (std::abs(_filter) > _options.resyncTicks)
        {
            // Too far for a gentle correction: advance the clock by the whole error once. Ahead: hold; behind: run extra.
            const auto ticks = static_cast<std::int32_t>(std::llround(-_filter));
            _pendingResync = ticks;
            ++_stats.resyncs;
            if (ticks < 0)
            {
                _stats.ticksHeld += static_cast<std::uint64_t>(-static_cast<std::int64_t>(ticks));
            }
            else
            {
                _stats.ticksSkipped += static_cast<std::uint64_t>(ticks);
            }
            _cooldownUntil = nowMicros + _options.resyncCooldownMicros;
            _filter = 0.0;
            _seeded = false;
            _correcting = false;
            _scale = 1.0;
            return;
        }

        const double magnitude = std::abs(_filter);
        if (!_correcting && magnitude > _options.deadZoneTicks)
        {
            _correcting = true;
        }
        else if (_correcting && magnitude < _options.deadZoneTicks * 0.5)
        {
            _correcting = false;
        }

        double desired = 1.0;
        if (_correcting)
        {
            const double excess = _filter - std::copysign(_options.deadZoneTicks, _filter);
            // Inside the dead zone while still correcting (hysteresis): no push, the scale just relaxes toward 1
            const double usable = (magnitude > _options.deadZoneTicks) ? excess : 0.0;
            desired = 1.0 - _options.gainPerTick * usable;
        }
        const double clamped = std::clamp(desired, _options.minScale, _options.maxScale);
        if (clamped != desired)
        {
            ++_stats.clampedUpdates;
        }
        MoveScale(clamped, seconds);
    }
}
