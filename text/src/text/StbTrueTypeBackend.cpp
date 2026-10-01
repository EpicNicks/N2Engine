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

        std::uint32_t ReadU32(const unsigned char *p)
        {
            return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }

        // stb_truetype is handed a pointer, not a size, and trusts the table directory. Checking that every
        // table lies inside the buffer first stops a truncated file from sending it past the end.
        bool TableDirectoryFits(const std::vector<unsigned char> &data, const std::size_t fontStart, std::string &error)
        {
            if (fontStart + 12 > data.size())
            {
                error = "font data is too short for an sfnt header";
                return false;
            }
            const unsigned char *header = data.data() + fontStart;
            const std::size_t tableCount = ReadU16(header + 4);
            if (fontStart + 12 + tableCount * 16 > data.size())
            {
                error = "font table directory runs past the end of the data";
                return false;
            }
            for (std::size_t i = 0; i < tableCount; ++i)
            {
                const unsigned char *record = header + 12 + i * 16;
                const std::uint64_t offset = ReadU32(record + 8);
                const std::uint64_t length = ReadU32(record + 12);
                if (offset + length > data.size())
                {
                    error = std::format("font table {} runs past the end of the data", i);
                    return false;
                }
            }
            return true;
        }

        struct TableSpan
        {
            std::size_t offset = 0;
            std::size_t length = 0;
        };

        // The first table with this tag, as stb_truetype finds it (the directory must already fit the data)
        std::optional<TableSpan> FindTable(const std::vector<unsigned char> &data, const std::size_t fontStart,
                                           const char (&tag)[5])
        {
            const unsigned char *header = data.data() + fontStart;
            const std::size_t tableCount = ReadU16(header + 4);
            for (std::size_t i = 0; i < tableCount; ++i)
            {
                const unsigned char *record = header + 12 + i * 16;
                if (std::memcmp(record, tag, 4) == 0)
                {
                    return TableSpan{ReadU32(record + 8), ReadU32(record + 12)};
                }
            }
            return std::nullopt;
        }

        // stb_truetype reads these tables at fixed offsets and indexes loca and hmtx by glyph id without
        // checking their lengths. Checking the sizes and the loca offsets up front turns a corrupt or cut-down
        // table into a load error instead of a read past the table (or the buffer). Runs before
        // stbtt_InitFont, which already reads head and maxp.
        bool TablesAreConsistent(const std::vector<unsigned char> &data, const std::size_t fontStart, std::string &error)
        {
            const auto fail = [&error](std::string reason)
            {
                error = std::move(reason);
                return false;
            };
            const unsigned char *bytes = data.data();

            const auto head = FindTable(data, fontStart, "head");
            if (!head || head->length < 54)
                return fail("the head table is missing or shorter than 54 bytes");
            const auto indexToLocFormat = static_cast<std::int16_t>(ReadU16(bytes + head->offset + 50));
            if (indexToLocFormat != 0 && indexToLocFormat != 1)
                return fail(std::format("head.indexToLocFormat is {} (must be 0 or 1)", indexToLocFormat));

            const auto maxp = FindTable(data, fontStart, "maxp");
            if (!maxp || maxp->length < 6)
                return fail("the maxp table is missing or too short");
            const std::size_t glyphCount = ReadU16(bytes + maxp->offset + 4);
            if (glyphCount < 1)
                return fail("the font has no glyphs (maxp.numGlyphs is 0)");

            const auto hhea = FindTable(data, fontStart, "hhea");
            if (!hhea || hhea->length < 36)
                return fail("the hhea table is missing or shorter than 36 bytes");
            const std::size_t hMetricCount = ReadU16(bytes + hhea->offset + 34);
            if (hMetricCount < 1 || hMetricCount > glyphCount)
                return fail(std::format("hhea.numberOfHMetrics is {} (must be 1 to {})", hMetricCount, glyphCount));

            const auto hmtx = FindTable(data, fontStart, "hmtx");
            if (!hmtx || hmtx->length < 4 * hMetricCount + 2 * (glyphCount - hMetricCount))
                return fail("the hmtx table is missing or too short for the glyph count");

            const auto glyf = FindTable(data, fontStart, "glyf");
            if (!glyf)
            {
                if (!FindTable(data, fontStart, "CFF "))
                    return fail("the font has no glyf or CFF outlines");
                return true; // CFF outlines have no loca table
            }

            const auto loca = FindTable(data, fontStart, "loca");
            const std::size_t entrySize = indexToLocFormat == 0 ? 2 : 4;
            if (!loca || loca->length < (glyphCount + 1) * entrySize)
                return fail("the loca table is missing or too short for the glyph count");
            std::size_t previous = 0;
            for (std::size_t i = 0; i <= glyphCount; ++i)
            {
                const unsigned char *entry = bytes + loca->offset + i * entrySize;
                const std::size_t offset = entrySize == 2 ? std::size_t{ReadU16(entry)} * 2 : std::size_t{ReadU32(entry)};
                if (offset < previous || offset > glyf->length)
                    return fail(std::format("loca entry {} is out of order or past the end of glyf", i));
                previous = offset;
            }
            return true;
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
                const int fontStart = stbtt_GetFontOffsetForIndex(_data.data(), 0);
                if (fontStart < 0)
                {
                    error = "not a TrueType or OpenType font";
                    return false;
                }
                if (!TableDirectoryFits(_data, static_cast<std::size_t>(fontStart), error) ||
                    !TablesAreConsistent(_data, static_cast<std::size_t>(fontStart), error))
                {
                    return false;
                }
                if (stbtt_InitFont(&_info, _data.data(), fontStart) == 0)
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
