// The one translation unit that compiles stb_truetype. STBTT_STATIC keeps every stb symbol private to
// this file, so no stb type or function leaks into the text library's interface or clashes with another
// copy of stb_truetype linked into the same program.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
// stb asserts on font data it can't handle; a bad font must fail to load (or render nothing), not abort
// Debug builds while Release carries on
#define STBTT_assert(x) ((void)0)
#include <stb_truetype/stb_truetype.h>

#include "StbTrueTypeBackend.hpp"
#include "SfntValidation.hpp"

#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace N2Engine::Text::Detail
{
    namespace
    {
        std::uint16_t ReadU16(const unsigned char *p)
        {
            return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        }

        class StbTrueTypeFace final : public IFontFace
        {
        public:
            explicit StbTrueTypeFace(std::vector<unsigned char> data) : _data(std::move(data)) {}

            bool Initialize(std::string &error)
            {
                if (_data.size() < 16 || _data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    error = "font data has an invalid size";
                    return false;
                }
                // The same checks as every backend (SfntValidation.hpp): the font start as
                // stbtt_GetFontOffsetForIndex(0) finds it, then the table directory and the tables stb indexes
                const auto fontStart = FindFirstFontOffset(_data);
                if (!fontStart || *fontStart > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                {
                    error = "not a TrueType or OpenType font";
                    return false;
                }
                if (!ValidateSfnt(_data, *fontStart, error))
                {
                    return false;
                }
                if (stbtt_InitFont(&_info, _data.data(), static_cast<int>(*fontStart)) == 0)
                {
                    error = "stb_truetype could not read the font (missing cmap, head, hhea, hmtx or outlines)";
                    return false;
                }

                _metrics.unitsPerEm = ReadU16(_data.data() + _info.head + 18);
                if (_metrics.unitsPerEm == 0)
                {
                    error = "font has zero units per em";
                    return false;
                }
                stbtt_GetFontVMetrics(&_info, &_metrics.ascent, &_metrics.descent, &_metrics.lineGap);
                return true;
            }

            [[nodiscard]] FontMetrics GetMetrics() const override { return _metrics; }

            [[nodiscard]] std::uint32_t GetGlyphCount() const override
            {
                return static_cast<std::uint32_t>(_info.numGlyphs);
            }

            [[nodiscard]] std::optional<GlyphId> FindGlyph(const char32_t codepoint) const override
            {
                if (codepoint > 0x10FFFF)
                {
                    return std::nullopt;
                }
                const int glyph = stbtt_FindGlyphIndex(&_info, static_cast<int>(codepoint));
                if (glyph <= 0 || glyph >= _info.numGlyphs)
                {
                    return std::nullopt;
                }
                return static_cast<GlyphId>(glyph);
            }

            [[nodiscard]] GlyphMetrics GetGlyphMetrics(const GlyphId glyph) const override
            {
                GlyphMetrics metrics;
                if (IsValid(glyph))
                {
                    stbtt_GetGlyphHMetrics(&_info, static_cast<int>(glyph), &metrics.advance, &metrics.leftSideBearing);
                }
                return metrics;
            }

            [[nodiscard]] std::optional<Rect> GetGlyphBounds(const GlyphId glyph) const override
            {
                int x0 = 0;
                int y0 = 0;
                int x1 = 0;
                int y1 = 0;
                if (!IsValid(glyph) || stbtt_GetGlyphBox(&_info, static_cast<int>(glyph), &x0, &y0, &x1, &y1) == 0 ||
                    x1 <= x0 || y1 <= y0)
                {
                    return std::nullopt;
                }
                return Rect{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1)};
            }

            [[nodiscard]] int GetKerning(const GlyphId left, const GlyphId right) const override
            {
                if (!IsValid(left) || !IsValid(right))
                {
                    return 0;
                }
                return stbtt_GetGlyphKernAdvance(&_info, static_cast<int>(left), static_cast<int>(right));
            }

            [[nodiscard]] GlyphSdf RenderSdf(const GlyphId glyph, const float pixelsPerEm, const int spreadPx) const override
            {
                GlyphSdf sdf;
                if (!IsValid(glyph) || !(pixelsPerEm > 0.0f) || spreadPx < 1)
                {
                    return sdf;
                }

                const float scale = stbtt_ScaleForMappingEmToPixels(&_info, pixelsPerEm);
                const float distanceScale = 128.0f / static_cast<float>(spreadPx);
                unsigned char *bitmap = stbtt_GetGlyphSDF(&_info, scale, static_cast<int>(glyph), spreadPx, 128,
                                                          distanceScale, &sdf.width, &sdf.height, &sdf.xOffset,
                                                          &sdf.yOffset);
                if (bitmap == nullptr)
                {
                    return GlyphSdf{}; // no outline: stb leaves the outputs unset
                }

                sdf.pixels.assign(bitmap, bitmap + static_cast<std::size_t>(sdf.width) * static_cast<std::size_t>(sdf.height));
                stbtt_FreeSDF(bitmap, _info.userdata);
                return sdf;
            }

        private:
            [[nodiscard]] bool IsValid(const GlyphId glyph) const
            {
                return glyph < static_cast<GlyphId>(_info.numGlyphs);
            }

            std::vector<unsigned char> _data; // stb_truetype reads the font in place, so the face owns it
            stbtt_fontinfo _info{};
            FontMetrics _metrics;
        };

        class StbTrueTypeBackend final : public IFontBackend
        {
        public:
            [[nodiscard]] FontBackendKind GetKind() const override { return FontBackendKind::StbTrueType; }

            [[nodiscard]] std::unique_ptr<IFontFace> LoadFace(const std::span<const std::byte> data,
                                                              std::string *error) const override
            {
                std::vector<unsigned char> copy(data.size());
                if (!data.empty())
                {
                    std::memcpy(copy.data(), data.data(), data.size());
                }

                auto face = std::make_unique<StbTrueTypeFace>(std::move(copy));
                std::string reason;
                if (!face->Initialize(reason))
                {
                    if (error != nullptr)
                    {
                        *error = std::move(reason);
                    }
                    return nullptr;
                }
                return face;
            }
        };
    }

    std::unique_ptr<IFontBackend> CreateStbTrueTypeBackend()
    {
        return std::make_unique<StbTrueTypeBackend>();
    }
}
