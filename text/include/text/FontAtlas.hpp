#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "text/FontBackend.hpp"
#include "text/Types.hpp"

namespace N2Engine::Text
{
    /// The base set of codepoints an atlas rasterises (extraChars adds to it)
    enum class Charset
    {
        Ascii,  // printable ASCII, U+0020-U+007E
        Latin1, // printable ASCII plus the Latin-1 Supplement, U+00A0-U+00FF
    };

    /// How a font's SDF atlas is generated. The engine reads these from a font's .meta customData.
    /// Limits are FontAtlas's kMin/kMax constants; settings outside them fail the build rather than being clamped.
    struct AtlasSettings
    {
        float basePx = 48.0f; // rasterisation size, in pixels per em (4 to 256)
        int spreadPx = 8;     // SDF distance range in pixels each side of the outline (1 to MaxSpreadPx(basePx))
        int paddingPx = 2;    // empty pixels between glyphs and around the atlas edge (stops filtering bleed)
        Charset charset = Charset::Latin1;
        std::u32string extraChars; // added to the charset; duplicates are ignored
    };

    /// The codepoints the settings ask for, sorted and unique. Always includes U+FFFD, which is what
    /// ill-formed UTF-8 decodes to.
    [[nodiscard]] std::vector<char32_t> ResolveCharset(const AtlasSettings &settings);

    /// The largest spreadPx allowed at this basePx: half of it, and at most FontAtlas::kMaxSpreadPx
    [[nodiscard]] int MaxSpreadPx(float basePx);

    struct AtlasGlyph
    {
        GlyphId glyph = 0;
        /// The codepoint that produced this glyph (0 for .notdef)
        char32_t codepoint = 0;
        /// Advance in ems (font units / unitsPerEm)
        float advance = 0.0f;
        /// The SDF bitmap's quad relative to the pen position on the baseline, in ems, y-up. Covers the
        /// spread border, which the shader needs. Empty for glyphs with no outline (spaces).
        Rect plane;
        /// Texture coordinates of the bitmap in the atlas, 0..1, y-down (minY is the top row)
        Rect uv;
        /// The bitmap's pixels in the atlas; width/height 0 for glyphs with no outline
        PixelRect pixels;

        [[nodiscard]] bool HasBitmap() const { return pixels.width > 0 && pixels.height > 0; }
    };

    /// A single-channel SDF atlas: one bitmap plus where each glyph sits in it. Built once (when a font
    /// loads) and immutable afterwards. The same font data, backend and settings always give a
    /// byte-identical atlas.
    class FontAtlas
    {
    public:
        /// Rasterises every charset codepoint the face has, plus .notdef. Codepoints the face lacks are
        /// skipped (they lay out as the fallback glyph). nullopt, with the reason in *error, if the
        /// settings are out of range, the charset has more than kMaxCodepoints codepoints, or the glyphs
        /// won't fit in kMaxSize x kMaxSize (estimated from the outline boxes before rasterising anything).
        [[nodiscard]] static std::optional<FontAtlas> Build(const IFontFace &face, const AtlasSettings &settings,
                                                            std::string *error = nullptr);

        static constexpr int kMaxSize = 8192;
        static constexpr float kMinBasePx = 4.0f;
        static constexpr float kMaxBasePx = 256.0f;
        static constexpr int kMaxSpreadPx = 32;
        static constexpr int kMaxPaddingPx = 16;
        /// Charsets larger than this fail before any glyph is looked up or rasterised
        static constexpr std::size_t kMaxCodepoints = 4096;

        [[nodiscard]] int GetWidth() const { return _width; }
        [[nodiscard]] int GetHeight() const { return _height; }
        /// GetWidth() * GetHeight() bytes, row 0 first; 128 is the glyph edge
        [[nodiscard]] const std::vector<std::uint8_t> &GetPixels() const { return _pixels; }
        [[nodiscard]] const AtlasSettings &GetSettings() const { return _settings; }

        /// Every glyph, sorted by glyph id
        [[nodiscard]] const std::vector<AtlasGlyph> &GetGlyphs() const { return _glyphs; }
        [[nodiscard]] const AtlasGlyph *FindGlyph(GlyphId glyph) const;
        /// The glyph for a codepoint, if the font has one and it's in the atlas
        [[nodiscard]] const AtlasGlyph *FindCodepoint(char32_t codepoint) const;
        /// The glyph id for a codepoint in the atlas (the same lookup as FindCodepoint)
        [[nodiscard]] std::optional<GlyphId> FindGlyphId(char32_t codepoint) const;

        /// What a missing codepoint draws as: the font's .notdef if it has an outline, otherwise '?',
        /// otherwise nullptr (the character then takes no space)
        [[nodiscard]] const AtlasGlyph *GetFallbackGlyph() const;

    private:
        FontAtlas() = default;

        int _width = 0;
        int _height = 0;
        std::vector<std::uint8_t> _pixels;
        AtlasSettings _settings;
        std::vector<AtlasGlyph> _glyphs;
        std::unordered_map<GlyphId, std::size_t> _glyphIndex;
        std::unordered_map<char32_t, GlyphId> _codepointToGlyph;
        std::optional<std::size_t> _fallback;
    };
}
