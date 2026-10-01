#pragma once

#include <math/Vector2.hpp>

namespace N2Engine::UI
{
    /**
     * An axis-aligned rectangle in canvas space: pixels (window coordinates' units), origin at the bottom-left
     * of the canvas, y up, as in Unity's screen space. (x, y) is the bottom-left corner.
     */
    struct Rect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        [[nodiscard]] constexpr float XMin() const { return x; }
        [[nodiscard]] constexpr float YMin() const { return y; }
        [[nodiscard]] constexpr float XMax() const { return x + width; }
        [[nodiscard]] constexpr float YMax() const { return y + height; }

        /// Whether it covers any area. A rect with a zero or negative size is neither drawn nor hit.
        [[nodiscard]] constexpr bool HasArea() const { return width > 0.0f && height > 0.0f; }

        /// Whether the point is inside, edges included (so neighbouring rects leave no gap). False for a rect
        /// without area.
        [[nodiscard]] bool Contains(const Math::Vector2 &point) const
        {
            return HasArea() && point.x >= XMin() && point.x <= XMax() && point.y >= YMin() && point.y <= YMax();
        }

        friend constexpr bool operator==(const Rect &, const Rect &) = default;
    };
}
