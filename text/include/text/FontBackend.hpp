#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "text/Types.hpp"

namespace N2Engine::Text
{
    /// Font-wide vertical metrics in font units (y-up: ascent > 0, descent usually < 0)
    struct FontMetrics
    {
        int ascent = 0;
        int descent = 0;
        int lineGap = 0;
        int unitsPerEm = 0;
    };

    /// Horizontal metrics of one glyph, in font units
    struct GlyphMetrics
    {
        int advance = 0;
        int leftSideBearing = 0;
    };

    /// A single-channel signed distance field of one glyph.
    /// The edge is 128; values above it are inside the outline. Each pixel of distance changes the value
    /// by 128 / spreadPx, so a pixel spreadPx or more outside the outline is 0. An empty glyph (a space)
    /// has width == height == 0 and no pixels.
    struct GlyphSdf
    {
        int width = 0;
        int height = 0;
        /// From the pen position on the baseline to the bitmap's top-left corner, in pixels, y-down
        /// (so a glyph above the baseline has a negative yOffset)
        int xOffset = 0;
        int yOffset = 0;
        std::vector<std::uint8_t> pixels; // width * height, row 0 at the top
    };

    /// One loaded font. Implementations own a copy of the font data, so the buffer it was loaded from
    /// can be released. Not thread-safe.
    class IFontFace
    {
    public:
        virtual ~IFontFace() = default;

        [[nodiscard]] virtual FontMetrics GetMetrics() const = 0;
        [[nodiscard]] virtual std::uint32_t GetGlyphCount() const = 0;

        /// The glyph the font maps the codepoint to, or nullopt if it has none (never .notdef)
        [[nodiscard]] virtual std::optional<GlyphId> FindGlyph(char32_t codepoint) const = 0;
        [[nodiscard]] virtual GlyphMetrics GetGlyphMetrics(GlyphId glyph) const = 0;
        /// Pair kerning in font units, added to the left glyph's advance (0 for no adjustment)
        [[nodiscard]] virtual int GetKerning(GlyphId left, GlyphId right) const = 0;

        /// Rasterises the glyph's distance field at pixelsPerEm pixels per em, with spreadPx pixels of
        /// distance (and as much border) around the outline. Empty for glyphs with no outline.
        [[nodiscard]] virtual GlyphSdf RenderSdf(GlyphId glyph, float pixelsPerEm, int spreadPx) const = 0;
    };

    /// The font backends the build can contain. P1b adds FreeType behind a CMake option.
    enum class FontBackendKind
    {
        StbTrueType,
        FreeType,
    };

    [[nodiscard]] std::string_view ToString(FontBackendKind kind);

    /// Creates font faces from font files (TrueType .ttf and CFF-flavoured .otf) in memory
    class IFontBackend
    {
    public:
        virtual ~IFontBackend() = default;

        [[nodiscard]] virtual FontBackendKind GetKind() const = 0;

        /// The face, or nullptr if the data isn't a font this backend can read (with the reason in
        /// *error when error isn't null). Never throws for bad data.
        [[nodiscard]] virtual std::unique_ptr<IFontFace> LoadFace(std::span<const std::byte> data,
                                                                  std::string *error = nullptr) const = 0;
    };

    /// The backends compiled into this build, default first
    [[nodiscard]] std::span<const FontBackendKind> GetAvailableFontBackends();
    [[nodiscard]] bool IsFontBackendAvailable(FontBackendKind kind);
    /// nullptr if this build doesn't contain the backend
    [[nodiscard]] std::unique_ptr<IFontBackend> CreateFontBackend(FontBackendKind kind);
    /// stb_truetype
    [[nodiscard]] FontBackendKind GetDefaultFontBackendKind();
    [[nodiscard]] std::unique_ptr<IFontBackend> CreateDefaultFontBackend();
}
