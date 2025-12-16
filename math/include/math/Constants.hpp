#pragma once

#include <numbers>

namespace N2Engine::Math::Constants
{
    constexpr float EPSILON = 0.00001f;
    constexpr float PI_F = std::numbers::pi_v<float>;
    constexpr float DEG_TO_RAD = PI_F / 180.0f;
    constexpr float RAD_TO_DEG = 180.0f / PI_F;
    constexpr float TWO_PI = 2.0f * PI_F;
    constexpr float HALF_PI = PI_F / 2.0f;
}