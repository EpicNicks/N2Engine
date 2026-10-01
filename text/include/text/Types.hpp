#pragma once

#include <cstdint>

namespace N2Engine::Text
{
    /// A glyph index inside one font face (0 is the font's .notdef glyph)
    using GlyphId = std::uint32_t;

    /// An axis-aligned rectangle. Which way y points depends on the space: layout positions and glyph
    /// plane rects are y-up, atlas texture coordinates are y-down (row 0 of the atlas at minY = 0).
    struct Rect
    {
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;

        [[nodiscard]] float Width() const { return maxX - minX; }
        [[nodiscard]] float Height() const { return maxY - minY; }
        [[nodiscard]] bool IsEmpty() const { return maxX <= minX || maxY <= minY; }

        bool operator==(const Rect &) const = default;
    };

    /// Integer pixel rectangle in an atlas bitmap (y-down, row 0 first)
    struct PixelRect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        bool operator==(const PixelRect &) const = default;
    };
}
