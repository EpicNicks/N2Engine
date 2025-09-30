#pragma once
#include <algorithm>
#include <cmath>
#include "math/Constants.hpp"

namespace N2Engine
{
    namespace Math
    {
        inline float Smoothstep(float edge0, float edge1, float x)
        {
            float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Ken Perlin's smoothstep version
        inline float Smootherstep(float edge0, float edge1, float x)
        {
            float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        inline bool Approximately(float a, float b, float epsilon = Constants::EPSILON)
        {
            return std::abs(a - b) < epsilon;
        }

        inline float Sign(float x)
        {
            return (float)((x > 0.0f) - (x < 0.0f));
        }

        inline int Sign(int x)
        {
            return (x > 0) - (x < 0);
        }

        inline float SignNonZero(float x)
        {
            return (x >= 0.0f) ? 1.0f : -1.0f;
        }

        inline float PingPong(float t, float length)
        {
            t = std::fmod(t, length * 2.0f);
            return length - std::abs(t - length);
        }

        // Repeat - wraps value between 0 and length
        inline float Repeat(float t, float length)
        {
            return std::clamp(t - std::floor(t / length) * length, 0.0f, length);
        }
    }
}