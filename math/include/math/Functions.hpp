#pragma once

#include <cmath>
#include <algorithm>

namespace N2Engine::Math::Functions
{
    inline float PingPong(float t, float length)
    {
        t = fmod(t, 2 * length);
        return length - std::abs(t - length);
    }

    inline float Clamp(float value, float min, float max)
    {
        return std::min(std::max(value, min), max);
    }

    inline float Clamp01(float value)
    {
        return Clamp(value, 0.0f, 1.0f);
    }

    // loops value back to 0 after reaching length
    inline float Repeat(float t, float length)
    {
        return fmod(t, length);
    }

    inline float Lerp(float a, float b, float t)
    {
        return a + (b - a) * Clamp01(t);
    }

    inline float LerpUnclamped(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    inline float MoveTowards(float current, float target, float maxDelta)
    {
        std::abs(target - current) <= maxDelta
            ? target
            : current + ((target > current) ? maxDelta : -maxDelta);
    }

    inline float DeltaAngle(float current, float target)
    {
        float delta = fmod((target - current), 360.0f);
        if (delta > 180.0f)
            delta -= 360.0f;
        if (delta < -180.0f)
            delta += 360.0f;
        return delta;
    }

    inline float LerpAngle(float a, float b, float t)
    {
        float delta = DeltaAngle(a, b);
        return a + delta * Clamp01(t);
    }

    inline bool Approximately(float a, float b, float epsilon = 1e-6f)
    {
        return std::abs(a - b) < epsilon;
    }

    inline int Sign(float value)
    {
        return (value >= 0.0f) ? 1 : -1;
    }
}