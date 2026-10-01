#include "text/Shaper.hpp"

#include "text/SdfFont.hpp"

namespace N2Engine::Text
{
    void BasicShaper::Shape(const SdfFont &font, const std::u32string_view codepoints,
                            const std::span<const std::uint32_t> clusters, std::vector<ShapedGlyph> &out) const
    {
        const FontAtlas &atlas = font.GetAtlas();
        const IFontFace &face = font.GetFace();
        const float unitsPerEm = static_cast<float>(font.GetMetrics().unitsPerEm);
        const AtlasGlyph *fallback = atlas.GetFallbackGlyph();

        const std::size_t first = out.size();
        for (std::size_t i = 0; i < codepoints.size(); ++i)
        {
            const char32_t codepoint = codepoints[i];
            const AtlasGlyph *entry = atlas.FindCodepoint(codepoint == U'\t' ? U' ' : codepoint);

            ShapedGlyph shaped;
            shaped.codepoint = codepoint;
            shaped.cluster = i < clusters.size() ? clusters[i] : 0;
            if (entry != nullptr)
            {
                shaped.glyph = entry->glyph;
                shaped.xAdvance = entry->advance;
            }
            else if (codepoint != U'\t')
            {
                shaped.missing = true;
                if (fallback != nullptr)
                {
                    shaped.glyph = fallback->glyph;
                    shaped.xAdvance = fallback->advance;
                }
            }
            out.push_back(shaped);
        }

        // Pair kerning is recorded on the left glyph. Tabs (whose width layout decides) and fallback glyphs
        // aren't kerned.
        for (std::size_t i = first; i + 1 < out.size(); ++i)
        {
            const ShapedGlyph &left = out[i];
            const ShapedGlyph &right = out[i + 1];
            if (left.missing || right.missing || left.codepoint == U'\t' || right.codepoint == U'\t')
            {
                continue;
            }
            out[i].kern = static_cast<float>(face.GetKerning(left.glyph, right.glyph)) / unitsPerEm;
        }
    }

    const IShaper &GetBasicShaper()
    {
        static const BasicShaper shaper{};
        return shaper;
    }
}
