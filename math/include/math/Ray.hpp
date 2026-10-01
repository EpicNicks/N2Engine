#pragma once

#include "math/Vector3.hpp"

namespace N2Engine::Math
{
    /// A half-line: origin plus a direction (Camera::ScreenPointToRay returns a unit direction)
    struct Ray
    {
        Vector3 origin{0.0f, 0.0f, 0.0f};
        Vector3 direction{0.0f, 0.0f, -1.0f};

        Ray() = default;
        Ray(const Vector3 &origin_, const Vector3 &direction_) : origin(origin_), direction(direction_) {}

        /// The point distance units along the ray
        [[nodiscard]] Vector3 GetPoint(const float distance) const { return origin + direction * distance; }
    };
}
