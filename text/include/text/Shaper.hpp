#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "text/Types.hpp"

namespace N2Engine::Text
{
    class SdfFont;

    /// One positioned glyph from a shaper. Distances are in ems.
    struct ShapedGlyph
    {
        GlyphId glyph = 0;
        /// The (first) codepoint of the cluster, which layout uses to find spaces, tabs and hyphens
        char32_t codepoint = 0;
        /// Byte offset of the cluster in the UTF-8 text
        std::uint32_t cluster = 0;
        /// Pen advance after this glyph, without kerning
        float xAdvance = 0.0f;
        /// Pair kerning with the next glyph. Kept apart from xAdvance because layout applies it only when
        /// the next glyph is on the same line: a line's last glyph isn't kerned against the next line.
        float kern = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
        /// The font (or its atlas) has no glyph for the codepoint; glyph is the fallback
        bool missing = false;
    };

    /// Turns a run of codepoints (one paragraph, no line breaks) into glyphs. This is the seam for a real
    /// shaper (HarfBuzz) later: ligatures, marks and bidi would happen here, and layout only consumes
    /// ShapedGlyphs. Every glyph a shaper returns should be in the font's atlas; anything else draws as
    /// the fallback glyph.
    class IShaper
    {
    public:
        virtual ~IShaper() = default;

        /// Appends the shaped run to out. clusters[i] is the byte offset of codepoints[i].
        virtual void Shape(const SdfFont &font, std::u32string_view codepoints,
                           std::span<const std::uint32_t> clusters, std::vector<ShapedGlyph> &out) const = 0;
    };

    /// No shaping: one glyph per codepoint, from the atlas's cmap, plus pair kerning. Codepoints the
    /// atlas lacks become the fallback glyph (marked missing). A tab shapes as a space; layout moves it
    /// to the next tab stop.
    class BasicShaper final : public IShaper
    {
    public:
        void Shape(const SdfFont &font, std::u32string_view codepoints, std::span<const std::uint32_t> clusters,
                   std::vector<ShapedGlyph> &out) const override;
    };

    /// The shared BasicShaper that layout uses when LayoutOptions::shaper is null
    [[nodiscard]] const IShaper &GetBasicShaper();
}
