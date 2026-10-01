// The optional FreeType font backend (CMake option N2ENGINE_TEXT_FREETYPE). Only this file sees FreeType's
// headers; the text library's interface stays library-neutral.
//
// It's held to the same contract as the stb_truetype backend, and gives the same numbers for the same font:
// - the same structural checks before FreeType sees the data (SfntValidation.hpp);
// - metrics, advances, bounds and kerning in font units, read unscaled (FT_LOAD_NO_SCALE,
//   FT_KERNING_UNSCALED) from the same tables (hhea, hmtx, kern), and left side bearings straight from
//   hmtx, as stb_truetype reads them (FreeType's horiBearingX is the outline's xMin, 0 for empty glyphs);
// - SDFs from FreeType's "sdf" renderer, which already uses the contract's convention (128 at the edge,
//   higher inside, 128 / spread per pixel), copied into the bitmap box stb_truetype would produce, so
//   both backends give identical sizes and offsets for every glyph.
//
// Known differences from stb_truetype, all outside the bundled default font:
// - kerning: FreeType reads only the legacy 'kern' table, while stb_truetype prefers GPOS pair kerning
//   when a font has it;
// - a TrueType glyph whose hmtx left side bearing differs from its glyf xMin has its outline (so its
//   bounds and SDF) shifted by the difference, as the TrueType spec places it (stb_truetype uses the raw
//   coordinates); the reported bearing is hmtx's in both;
// - composite glyphs and glyphs flagged as having overlapping contours render with the sdf module's
//   overlap mode (simple glyphs use its default mode, which handles nested contours better);
// - RenderSdf supports spreads from 1 to 32 pixels (FreeType's renderer takes 2 to 32; a spread of 1 is
//   rendered at 2 and rescaled). Larger spreads render nothing; the atlas never asks for more than 32.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include "FreeTypeBackend.hpp"
#include "SfntValidation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <utility>

namespace N2Engine::Text::Detail
{
    namespace
    {
        // FreeType's SDF renderer accepts spreads in this range (MIN_SPREAD and MAX_SPREAD in ftsdfcommon.h)
        constexpr int kFreeTypeMinSpread = 2;
        constexpr int kFreeTypeMaxSpread = 32;

        std::uint16_t ReadU16(const unsigned char *p)
        {
            return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
        }

        // Glyphs are loaded in font units, without hinting or embedded bitmaps
        constexpr FT_Int32 kLoadFlags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;

        class FreeTypeFace final : public IFontFace
        {
        public:
            explicit FreeTypeFace(std::vector<unsigned char> data) : _data(std::move(data)) {}

            ~FreeTypeFace() override
            {
                if (_face != nullptr)
                {
                    FT_Done_Face(_face);
                }
                if (_library != nullptr)
                {
                    FT_Done_FreeType(_library);
                }
            }

            FreeTypeFace(const FreeTypeFace &) = delete;
            FreeTypeFace &operator=(const FreeTypeFace &) = delete;

            bool Initialize(std::string &error)
            {
                if (_data.size() < 16 || _data.size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()))
                {
                    error = "font data has an invalid size";
                    return false;
                }
                const auto fontStart = FindFirstFontOffset(_data);
                if (!fontStart)
                {
                    error = "not a TrueType or OpenType font";
                    return false;
                }
                if (!ValidateSfnt(_data, *fontStart, error))
                {
                    return false;
                }
                // Validated above: both tables exist, and hmtx holds numberOfHMetrics long entries plus a
                // bearing for every remaining glyph
                const auto hheaTable = FindSfntTable(_data, *fontStart, "hhea");
                const auto hmtxTable = FindSfntTable(_data, *fontStart, "hmtx");
                if (!hheaTable || !hmtxTable)
                {
                    error = "the hhea or hmtx table is missing";
                    return false;
                }
                _hmtx = *hmtxTable;
                _hMetricCount = ReadU16(_data.data() + hheaTable->offset + 34);

                // One library per face: faces are independent and can live on different threads
                if (const FT_Error result = FT_Init_FreeType(&_library); result != 0)
                {
                    _library = nullptr;
                    error = std::format("FreeType failed to initialise (error {})", result);
                    return false;
                }
                // FreeType reads the face in place, so _data must outlive _face (the destructor frees it first)
                if (const FT_Error result = FT_New_Memory_Face(_library, _data.data(), static_cast<FT_Long>(_data.size()),
                                                               0, &_face);
                    result != 0)
                {
                    _face = nullptr;
                    error = std::format("FreeType could not read the font (error {})", result);
                    return false;
                }
                if (!FT_IS_SFNT(_face) || !FT_IS_SCALABLE(_face))
                {
                    error = "the font has no scalable outlines";
                    return false;
                }
                // FreeType selects a Unicode cmap by itself when there is one; stb_truetype also needs one
                if (_face->charmap == nullptr || _face->charmap->encoding != FT_ENCODING_UNICODE)
                {
                    if (FT_Select_Charmap(_face, FT_ENCODING_UNICODE) != 0)
                    {
                        error = "the font has no Unicode cmap";
                        return false;
                    }
                }
                if (_face->num_glyphs < 1)
                {
                    error = "the font has no glyphs";
                    return false;
                }

                _metrics.unitsPerEm = _face->units_per_EM;
                if (_metrics.unitsPerEm == 0)
                {
                    error = "font has zero units per em";
                    return false;
                }
                // hhea, as stb_truetype reads it (FT_Face::ascender falls back to OS/2 when hhea is zero)
                const auto *hhea = static_cast<const TT_HoriHeader *>(FT_Get_Sfnt_Table(_face, FT_SFNT_HHEA));
                if (hhea == nullptr)
                {
                    error = "the font has no hhea table";
                    return false;
                }
                _metrics.ascent = hhea->Ascender;
                _metrics.descent = hhea->Descender;
                _metrics.lineGap = hhea->Line_Gap;
                return true;
            }

            [[nodiscard]] FontMetrics GetMetrics() const override { return _metrics; }

            [[nodiscard]] std::uint32_t GetGlyphCount() const override
            {
                return static_cast<std::uint32_t>(_face->num_glyphs);
            }

            [[nodiscard]] std::optional<GlyphId> FindGlyph(const char32_t codepoint) const override
            {
                if (codepoint > 0x10FFFF)
                {
                    return std::nullopt;
                }
                const FT_UInt glyph = FT_Get_Char_Index(_face, static_cast<FT_ULong>(codepoint));
                if (glyph == 0 || !IsValid(glyph))
                {
                    return std::nullopt;
                }
                return static_cast<GlyphId>(glyph);
            }

            [[nodiscard]] GlyphMetrics GetGlyphMetrics(const GlyphId glyph) const override
            {
                GlyphMetrics metrics;
                if (!IsValid(glyph))
                {
                    return metrics;
                }
                // With FT_LOAD_NO_SCALE the advance is in font units, not 16.16
                FT_Fixed advance = 0;
                if (FT_Get_Advance(_face, glyph, kLoadFlags, &advance) == 0)
                {
                    metrics.advance = static_cast<int>(advance);
                }
                metrics.leftSideBearing = ReadLeftSideBearing(glyph);
                return metrics;
            }

            [[nodiscard]] std::optional<Rect> GetGlyphBounds(const GlyphId glyph) const override
            {
                const auto box = LoadOutlineBox(glyph);
                if (!box)
                {
                    return std::nullopt;
                }
                return Rect{static_cast<float>(box->xMin), static_cast<float>(box->yMin), static_cast<float>(box->xMax),
                            static_cast<float>(box->yMax)};
            }

            [[nodiscard]] int GetKerning(const GlyphId left, const GlyphId right) const override
            {
                if (!IsValid(left) || !IsValid(right) || !FT_HAS_KERNING(_face))
                {
                    return 0;
                }
                FT_Vector kerning{};
                if (FT_Get_Kerning(_face, left, right, FT_KERNING_UNSCALED, &kerning) != 0)
                {
                    return 0;
                }
                return static_cast<int>(kerning.x);
            }

            [[nodiscard]] GlyphSdf RenderSdf(const GlyphId glyph, const float pixelsPerEm, const int spreadPx) const override
            {
                if (!(pixelsPerEm > 0.0f) || !std::isfinite(pixelsPerEm) || spreadPx < 1 || spreadPx > kFreeTypeMaxSpread)
                {
                    return {};
                }
                // Checked first: it loads the glyph's unexpanded form into the slot
                const bool composite = IsComposite(glyph);
                // Loads the glyph into the slot, unscaled
                const auto box = LoadOutlineBox(glyph);
                if (!box)
                {
                    return {}; // no outline (a space) or an invalid glyph
                }

                // The bitmap box exactly as stb_truetype computes it (stbtt_GetGlyphBitmapBoxSubpixel, float
                // maths): the outline box scaled, rounded outwards to whole pixels, y-down, plus the spread
                const float scale = pixelsPerEm / static_cast<float>(_metrics.unitsPerEm);
                const double fixedScale = static_cast<double>(scale) * 64.0 * 65536.0; // font units -> 26.6, as 16.16
                if (!(fixedScale >= 1.0) || fixedScale > static_cast<double>(std::numeric_limits<std::int32_t>::max()))
                {
                    return {}; // FT_Fixed is 32-bit on Windows
                }
                const double x0 = std::floor(static_cast<float>(box->xMin) * scale);
                const double y0 = std::floor(-static_cast<float>(box->yMax) * scale);
                const double x1 = std::ceil(static_cast<float>(box->xMax) * scale);
                const double y1 = std::ceil(-static_cast<float>(box->yMin) * scale);
                if (x0 == x1 || y0 == y1)
                {
                    return {};
                }
                // FreeType's rasteriser stops at 16-bit pixel coordinates; past that there's nothing to draw
                constexpr double kLimit = 0x7000;
                if (x0 < -kLimit || y0 < -kLimit || x1 > kLimit || y1 > kLimit)
                {
                    return {};
                }

                GlyphSdf sdf;
                sdf.xOffset = static_cast<int>(x0) - spreadPx;
                sdf.yOffset = static_cast<int>(y0) - spreadPx;
                sdf.width = static_cast<int>(x1 - x0) + 2 * spreadPx;
                sdf.height = static_cast<int>(y1 - y0) + 2 * spreadPx;

                // Scale the unscaled outline ourselves rather than through a FreeType size, which would round
                // the ppem to an integer for fonts that ask for it (head.flags bit 3)
                const auto fixed = static_cast<FT_Fixed>(std::lround(fixedScale));
                const FT_Matrix matrix{fixed, 0, 0, fixed};
                FT_Outline_Transform(&_face->glyph->outline, &matrix);

                const int renderSpread = std::max(spreadPx, kFreeTypeMinSpread);
                // The sdf module's default mode takes the inside/outside sign from the nearest edge, which goes
                // wrong where contours overlap: stb_truetype (non-zero winding) fills the overlap, FreeType's
                // default mode gave Noto's U+00C7 and U+00E7 (C plus a cedilla component that overlaps it) pixels
                // 1.5 px outside where stb has them 1.5 px inside. Its overlap mode handles that, but gets
                // nested simple contours such as U+00A9 and U+00AE wrong, so it's used only where overlaps are
                // possible: composite glyphs (components may overlap) and glyphs flagged FT_OUTLINE_OVERLAP
                // (common in variable-font instances).
                const bool overlaps = composite || (_face->glyph->outline.flags & FT_OUTLINE_OVERLAP) != 0;
                if (!SetSpread(renderSpread) || !SetOverlaps(overlaps) ||
                    FT_Render_Glyph(_face->glyph, FT_RENDER_MODE_SDF) != 0)
                {
                    return {};
                }
                const FT_Bitmap &bitmap = _face->glyph->bitmap;
                if (bitmap.buffer == nullptr || bitmap.rows == 0 || bitmap.width == 0 ||
                    bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
                {
                    return {};
                }

                // Copy FreeType's bitmap into the stb-sized box. The two boxes can differ by a pixel at an edge,
                // because FreeType rounds the outline to 1/64 pixel before taking its box; the pixels FreeType
                // didn't produce are at least the spread outside the outline, so they're 0. FreeType's pitch is
                // the step to the next row down; when negative, the buffer starts at the bottom row.
                const int ftLeft = _face->glyph->bitmap_left; // pixels from the origin, x right
                const int ftTop = -_face->glyph->bitmap_top;  // y-down, like the contract's yOffset
                const int ftWidth = static_cast<int>(bitmap.width);
                const int ftRows = static_cast<int>(bitmap.rows);
                const std::ptrdiff_t pitch = bitmap.pitch;
                const unsigned char *topRow = bitmap.buffer + (pitch < 0 ? -pitch * (ftRows - 1) : 0);

                sdf.pixels.assign(static_cast<std::size_t>(sdf.width) * static_cast<std::size_t>(sdf.height), 0);
                for (int y = 0; y < sdf.height; ++y)
                {
                    const int fy = sdf.yOffset + y - ftTop;
                    if (fy < 0 || fy >= ftRows)
                    {
                        continue;
                    }
                    const unsigned char *row = topRow + pitch * fy;
                    for (int x = 0; x < sdf.width; ++x)
                    {
                        const int fx = sdf.xOffset + x - ftLeft;
                        if (fx < 0 || fx >= ftWidth)
                        {
                            continue;
                        }
                        int value = row[fx];
                        if (renderSpread != spreadPx)
                        {
                            // Rendered with a wider spread: stretch the distances back to 128 / spreadPx per pixel
                            value = std::clamp(128 + (value - 128) * renderSpread / spreadPx, 0, 255);
                        }
                        sdf.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(sdf.width) + x] =
                            static_cast<std::uint8_t>(value);
                    }
                }
                return sdf;
            }

        private:
            [[nodiscard]] bool IsValid(const GlyphId glyph) const
            {
                return glyph < static_cast<GlyphId>(_face->num_glyphs);
            }

            // True for a TrueType composite glyph (one built from other glyphs). Leaves the slot holding the
            // unexpanded glyph, so load the outline afterwards.
            [[nodiscard]] bool IsComposite(const GlyphId glyph) const
            {
                return IsValid(glyph) && FT_Load_Glyph(_face, glyph, kLoadFlags | FT_LOAD_NO_RECURSE) == 0 &&
                       _face->glyph->format == FT_GLYPH_FORMAT_COMPOSITE;
            }

            // The hmtx left side bearing, read the way stb_truetype reads it: from the glyph's long entry, or
            // from the bearing array that follows the long entries for glyphs past numberOfHMetrics
            [[nodiscard]] int ReadLeftSideBearing(const GlyphId glyph) const
            {
                const std::size_t index = glyph;
                const std::size_t at = index < _hMetricCount ? 4 * index + 2 : 4 * _hMetricCount + 2 * (index - _hMetricCount);
                if (at + 2 > _hmtx.length)
                {
                    return 0;
                }
                return static_cast<std::int16_t>(ReadU16(_data.data() + _hmtx.offset + at));
            }

            // Loads the glyph's outline unscaled into the face's slot and returns its control box in font
            // units (y-up), or nullopt for an invalid glyph or one with no outline
            [[nodiscard]] std::optional<FT_BBox> LoadOutlineBox(const GlyphId glyph) const
            {
                if (!IsValid(glyph) || FT_Load_Glyph(_face, glyph, kLoadFlags) != 0)
                {
                    return std::nullopt;
                }
                const FT_GlyphSlot slot = _face->glyph;
                if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->outline.n_points == 0 || slot->outline.n_contours == 0)
                {
                    return std::nullopt;
                }
                FT_BBox box{};
                FT_Outline_Get_CBox(&slot->outline, &box);
                if (box.xMax <= box.xMin || box.yMax <= box.yMin)
                {
                    return std::nullopt;
                }
                return box;
            }

            // The sdf module's spread is a library-wide property; it's only set when it changes
            // Like the spread, the overlaps switch is library-wide and only set when it changes
            [[nodiscard]] bool SetOverlaps(const bool overlaps) const
            {
                if (overlaps == _overlaps)
                {
                    return true;
                }
                const FT_Bool value = overlaps ? 1 : 0;
                if (FT_Property_Set(_library, "sdf", "overlaps", &value) != 0)
                {
                    return false;
                }
                _overlaps = overlaps;
                return true;
            }

            [[nodiscard]] bool SetSpread(const int spread) const
            {
                if (spread == _spread)
                {
                    return true;
                }
                const FT_Int value = spread;
                if (FT_Property_Set(_library, "sdf", "spread", &value) != 0)
                {
                    return false;
                }
                _spread = spread;
                return true;
            }

            std::vector<unsigned char> _data; // FreeType reads the font in place, so the face owns it
            SfntTable _hmtx;
            std::size_t _hMetricCount = 0;
            FT_Library _library = nullptr;
            FT_Face _face = nullptr;
            FontMetrics _metrics;
            mutable int _spread = -1; // the spread last set on the sdf module (-1: FreeType's default)
            mutable bool _overlaps = false; // the sdf module's overlaps property (off by default)
        };

        class FreeTypeBackend final : public IFontBackend
        {
        public:
            [[nodiscard]] FontBackendKind GetKind() const override { return FontBackendKind::FreeType; }

            [[nodiscard]] std::unique_ptr<IFontFace> LoadFace(const std::span<const std::byte> data,
                                                              std::string *error) const override
            {
                std::vector<unsigned char> copy(data.size());
                if (!data.empty())
                {
                    std::memcpy(copy.data(), data.data(), data.size());
                }

                auto face = std::make_unique<FreeTypeFace>(std::move(copy));
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

    std::unique_ptr<IFontBackend> CreateFreeTypeBackend()
    {
        return std::make_unique<FreeTypeBackend>();
    }
}
