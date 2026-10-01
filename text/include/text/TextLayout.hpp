#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "text/Types.hpp"

namespace N2Engine::Text
{
    class IShaper;
    class SdfFont;

    /// Where lines sit relative to the layout origin (the pivot): Left starts each line at x = 0,
    /// Center centres it on x = 0, Right ends it at x = 0
    enum class HorizontalAlign
    {
        Left,
        Center,
        Right,
    };

    /// Where the block sits relative to y = 0: Top puts the first line's ascent there, Middle centres the
    /// block, Bottom puts the last line's descent there, Baseline puts the first line's baseline there
    enum class VerticalAlign
    {
        Top,
        Middle,
        Bottom,
        Baseline,
    };

    /// fontSize, maxWidth, lineSpacing and tabWidth below 0 (or NaN) are treated as 0. letterSpacing may be
    /// negative, to tighten text; a non-finite one is treated as 0.
    struct LayoutOptions
    {
        /// Layout units per em (world units for world-space text, pixels for UI)
        float fontSize = 1.0f;
        /// Wrap width in layout units; 0 or less never wraps
        float maxWidth = 0.0f;
        HorizontalAlign horizontalAlign = HorizontalAlign::Left;
        VerticalAlign verticalAlign = VerticalAlign::Top;
        /// Multiplies the font's line height (ascent - descent + lineGap)
        float lineSpacing = 1.0f;
        /// Extra space between glyphs, in ems
        float letterSpacing = 0.0f;
        /// Tab stops every tabWidth space widths from the line start
        float tabWidth = 4.0f;
        /// nullptr uses GetBasicShaper()
        const IShaper *shaper = nullptr;
    };

    /// One glyph's quad, ready for a mesh: four corners from position, with matching texture coordinates
    /// from uv. position is y-up and uv is y-down, so the top-left corner is (position.minX,
    /// position.maxY) with uv (uv.minX, uv.minY).
    struct GlyphQuad
    {
        Rect position;
        Rect uv;
        char32_t codepoint = 0;
        /// Byte offset of the glyph's source text in the UTF-8 input
        std::uint32_t cluster = 0;
    };

    struct LineInfo
    {
        /// The line's quads are quads[firstQuad, firstQuad + quadCount)
        std::size_t firstQuad = 0;
        std::size_t quadCount = 0;
        /// The UTF-8 byte range the line covers, trailing spaces included and the line break excluded
        std::uint32_t byteBegin = 0;
        std::uint32_t byteEnd = 0;
        /// Left edge after alignment, baseline y, and width without trailing spaces (layout units)
        float x = 0.0f;
        float baseline = 0.0f;
        float width = 0.0f;
    };

    struct TextLayout
    {
        /// Only glyphs with a bitmap get a quad: spaces and tabs take room but have none
        std::vector<GlyphQuad> quads;
        std::vector<LineInfo> lines;
        /// The box the lines fill: from the widest line's edges, and from the first line's ascent to the
        /// last line's descent. All zero for empty text.
        Rect bounds;
        /// Distance between consecutive baselines (layout units)
        float lineHeight = 0.0f;
        /// Codepoints the font couldn't draw, in order of first appearance (drawn as the fallback glyph)
        std::vector<char32_t> missingCodepoints;
    };

    /// Lays out UTF-8 text (ill-formed bytes show as U+FFFD). "\n" and "\r\n" break lines; other control
    /// characters are ignored. With maxWidth > 0, lines wrap after the last space, tab or hyphen that
    /// fits; a word wider than the line breaks between characters. Trailing spaces never count towards
    /// a line's width or cause a wrap. Never throws for any input.
    [[nodiscard]] TextLayout LayoutText(const SdfFont &font, std::string_view utf8, const LayoutOptions &options = {});
}
