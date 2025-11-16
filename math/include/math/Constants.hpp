#pragma once

#include <numbers>

namespace N2Engine::Math::Constants
{
    constexpr float EPSILON = 0.00001f;
    constexpr float DEG_TO_RAD = std::numbers::pi_v<float> / 180.0f;
    constexpr float RAD_TO_DEG = 180.0f / std::numbers::pi_v<float>;
    constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;
    constexpr float HALF_PI = std::numbers::pi_v<float> / 2.0f;
}