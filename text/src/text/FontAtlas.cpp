#include "text/FontAtlas.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <utility>

#include "text/Utf8.hpp"

namespace N2Engine::Text
{
    namespace
    {
        bool IsControl(const char32_t c)
        {
            return c < 0x20 || (c >= 0x7F && c <= 0x9F);
        }

        bool IsSurrogate(const char32_t c)
        {
            return c >= 0xD800 && c <= 0xDFFF;
        }

        int NextPowerOfTwo(const int value)
        {
            int result = 1;
            while (result < value)
            {
                result *= 2;
            }
            return result;
        }

        struct PendingGlyph
        {
            AtlasGlyph glyph;
            GlyphSdf sdf;
        };

        // Shelf packer: fills rows left to right, tallest glyphs first. The order is fully determined by
        // (height, width, glyph id), so the layout never depends on hash or allocation order.
        // Returns the packed height (before rounding), or -1 if a glyph is wider than the atlas.
        int PackShelves(std::vector<PendingGlyph *> &order, const int width, const int padding)
        {
            int x = padding;
            int y = padding;
            int shelfHeight = 0;
            for (PendingGlyph *pending : order)
            {
                PixelRect &rect = pending->glyph.pixels;
                if (rect.width + 2 * padding > width)
                {
                    return -1;
                }
                if (x + rect.width + padding > width)
                {
                    y += shelfHeight + padding;
                    x = padding;
                    shelfHeight = 0;
                }
                rect.x = x;
                rect.y = y;
                x += rect.width + padding;
                shelfHeight = std::max(shelfHeight, rect.height);
            }
            return y + shelfHeight + padding;
        }
    }

    int MaxSpreadPx(const float basePx)
    {
        // A spread beyond half the em only blurs, and costs as much as a bigger basePx
        return std::clamp(static_cast<int>(basePx / 2.0f), 1, FontAtlas::kMaxSpreadPx);
    }

    std::vector<char32_t> ResolveCharset(const AtlasSettings &settings)
    {
        // U+FFFD is always there: DecodeUtf8 turns ill-formed bytes into it, and they should draw as the
        // replacement character, not as a missing glyph
        std::vector<char32_t> result{kReplacementCharacter};
        for (char32_t c = 0x20; c <= 0x7E; ++c)
        {
            result.push_back(c);
        }
        if (settings.charset == Charset::Latin1)
        {
            for (char32_t c = 0xA0; c <= 0xFF; ++c)
            {
                result.push_back(c);
            }
        }
        for (const char32_t c : settings.extraChars)
        {
            // Control characters never draw, and these can't be in a font's cmap
            if (!IsControl(c) && !IsSurrogate(c) && c <= 0x10FFFF)
            {
                result.push_back(c);
            }
        }
        std::ranges::sort(result);
        const auto duplicates = std::ranges::unique(result);
        result.erase(duplicates.begin(), duplicates.end());
        return result;
    }

    std::optional<FontAtlas> FontAtlas::Build(const IFontFace &face, const AtlasSettings &settings, std::string *error)
    {
        const auto fail = [error](std::string reason) -> std::optional<FontAtlas>
        {
            if (error != nullptr)
            {
                *error = std::move(reason);
            }
            return std::nullopt;
        };

        // SDF rasterisation costs about (glyph pixels x outline edges); these caps keep a load to seconds
        if (!(settings.basePx >= kMinBasePx && settings.basePx <= kMaxBasePx))
        {
            return fail(std::format("basePx must be between {} and {} (got {})", kMinBasePx, kMaxBasePx, settings.basePx));
        }
        const int maxSpread = MaxSpreadPx(settings.basePx);
        if (settings.spreadPx < 1 || settings.spreadPx > maxSpread)
        {
            return fail(std::format("spreadPx must be between 1 and {} at basePx {} (got {})", maxSpread,
                                    settings.basePx, settings.spreadPx));
        }
        if (settings.paddingPx < 0 || settings.paddingPx > kMaxPaddingPx)
        {
            return fail(std::format("paddingPx must be between 0 and {} (got {})", kMaxPaddingPx, settings.paddingPx));
        }

        const FontMetrics metrics = face.GetMetrics();
        if (metrics.unitsPerEm <= 0)
        {
            return fail("the font has no units per em");
        }

        // Glyph id -> first codepoint that maps to it. Ordered, so everything below is deterministic.
        std::map<GlyphId, char32_t> glyphCodepoints;
        glyphCodepoints.emplace(GlyphId{0}, char32_t{0}); // .notdef, the missing-glyph fallback
        std::unordered_map<char32_t, GlyphId> codepointToGlyph;
        const std::vector<char32_t> charset = ResolveCharset(settings);
        if (charset.size() > kMaxCodepoints)
        {
            return fail(std::format("the charset has {} codepoints; at most {} are allowed", charset.size(), kMaxCodepoints));
        }
        for (const char32_t c : charset)
        {
            if (const auto glyph = face.FindGlyph(c))
            {
                glyphCodepoints.emplace(*glyph, c); // keeps the lowest codepoint for a shared glyph
                codepointToGlyph.emplace(c, *glyph);
            }
        }

        const float unitsPerEm = static_cast<float>(metrics.unitsPerEm);

        // Estimate the packed area from the outline boxes before rasterising anything, so an atlas that
        // can't fit fails at once rather than after rendering every glyph
        {
            const double scale = settings.basePx / unitsPerEm;
            const double border = 2.0 * settings.spreadPx + 2.0; // spread each side, plus pixel rounding
            double estimatedArea = 0.0;
            for (const auto &[glyphId, codepoint] : glyphCodepoints)
            {
                if (const auto bounds = face.GetGlyphBounds(glyphId))
                {
                    estimatedArea += (bounds->Width() * scale + border + settings.paddingPx) *
                                     (bounds->Height() * scale + border + settings.paddingPx);
                }
            }
            if (estimatedArea > static_cast<double>(kMaxSize) * kMaxSize)
            {
                return fail(std::format("the glyphs need about {:.0f} pixels, more than a {}x{} atlas holds; lower "
                                        "basePx or the charset", estimatedArea, kMaxSize, kMaxSize));
            }
        }

        std::vector<PendingGlyph> pending;
        pending.reserve(glyphCodepoints.size());
        for (const auto &[glyphId, codepoint] : glyphCodepoints)
        {
            PendingGlyph entry;
            entry.glyph.glyph = glyphId;
            entry.glyph.codepoint = codepoint;
            entry.glyph.advance = static_cast<float>(face.GetGlyphMetrics(glyphId).advance) / unitsPerEm;
            entry.sdf = face.RenderSdf(glyphId, settings.basePx, settings.spreadPx);
            if (entry.sdf.width > 0 && entry.sdf.height > 0 &&
                entry.sdf.pixels.size() == static_cast<std::size_t>(entry.sdf.width) * static_cast<std::size_t>(entry.sdf.height))
            {
                entry.glyph.pixels.width = entry.sdf.width;
                entry.glyph.pixels.height = entry.sdf.height;
                const float px = settings.basePx;
                entry.glyph.plane.minX = static_cast<float>(entry.sdf.xOffset) / px;
                entry.glyph.plane.maxX = static_cast<float>(entry.sdf.xOffset + entry.sdf.width) / px;
                entry.glyph.plane.maxY = static_cast<float>(-entry.sdf.yOffset) / px;
                entry.glyph.plane.minY = static_cast<float>(-(entry.sdf.yOffset + entry.sdf.height)) / px;
            }
            pending.push_back(std::move(entry));
        }

        // Pack the glyphs that have bitmaps
        std::vector<PendingGlyph *> order;
        long long area = 0;
        int widest = 0;
        for (PendingGlyph &entry : pending)
        {
            if (entry.glyph.HasBitmap())
            {
                order.push_back(&entry);
                area += static_cast<long long>(entry.glyph.pixels.width + settings.paddingPx) *
                        (entry.glyph.pixels.height + settings.paddingPx);
                widest = std::max(widest, entry.glyph.pixels.width);
            }
        }
        std::ranges::sort(order, [](const PendingGlyph *a, const PendingGlyph *b)
        {
            if (a->glyph.pixels.height != b->glyph.pixels.height)
                return a->glyph.pixels.height > b->glyph.pixels.height;
            if (a->glyph.pixels.width != b->glyph.pixels.width)
                return a->glyph.pixels.width > b->glyph.pixels.width;
            return a->glyph.glyph < b->glyph.glyph;
        });

        // Square-ish power-of-two sizes: start from the smallest width whose square holds the area, and
        // widen while the packed height overshoots the width
        int width = std::max(64, NextPowerOfTwo(widest + 2 * settings.paddingPx));
        while (static_cast<long long>(width) * width < area && width < kMaxSize)
        {
            width *= 2;
        }
        int packedHeight = PackShelves(order, width, settings.paddingPx);
        while (packedHeight > width && width < kMaxSize)
        {
            width *= 2;
            packedHeight = PackShelves(order, width, settings.paddingPx);
        }
        if (packedHeight < 0 || width > kMaxSize)
        {
            return fail(std::format("a glyph is wider than the largest atlas ({} px)", kMaxSize));
        }
        const int height = NextPowerOfTwo(std::max(packedHeight, 1));
        if (height > kMaxSize)
        {
            return fail(std::format("the glyphs don't fit in a {0}x{0} atlas; lower basePx or the charset", kMaxSize));
        }

        FontAtlas atlas;
        atlas._width = width;
        atlas._height = height;
        atlas._settings = settings;
        atlas._pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);

        const float atlasWidth = static_cast<float>(width);
        const float atlasHeight = static_cast<float>(height);
        for (PendingGlyph *entry : order)
        {
            const PixelRect &rect = entry->glyph.pixels;
            for (int row = 0; row < rect.height; ++row)
            {
                std::memcpy(atlas._pixels.data() + static_cast<std::size_t>(rect.y + row) * width + rect.x,
                            entry->sdf.pixels.data() + static_cast<std::size_t>(row) * rect.width,
                            static_cast<std::size_t>(rect.width));
            }
            entry->glyph.uv.minX = static_cast<float>(rect.x) / atlasWidth;
            entry->glyph.uv.minY = static_cast<float>(rect.y) / atlasHeight;
            entry->glyph.uv.maxX = static_cast<float>(rect.x + rect.width) / atlasWidth;
            entry->glyph.uv.maxY = static_cast<float>(rect.y + rect.height) / atlasHeight;
        }

        atlas._glyphs.reserve(pending.size());
        for (PendingGlyph &entry : pending) // already in glyph id order (from the map)
        {
            atlas._glyphIndex.emplace(entry.glyph.glyph, atlas._glyphs.size());
            atlas._glyphs.push_back(entry.glyph);
        }
        atlas._codepointToGlyph = std::move(codepointToGlyph);

        if (const AtlasGlyph *notdef = atlas.FindGlyph(0); notdef != nullptr && notdef->HasBitmap())
        {
            atlas._fallback = atlas._glyphIndex.at(0);
        }
        else if (const auto question = atlas.FindGlyphId(U'?'))
        {
            atlas._fallback = atlas._glyphIndex.at(*question);
        }

        return atlas;
    }

    const AtlasGlyph *FontAtlas::FindGlyph(const GlyphId glyph) const
    {
        const auto it = _glyphIndex.find(glyph);
        return it != _glyphIndex.end() ? &_glyphs[it->second] : nullptr;
    }

    const AtlasGlyph *FontAtlas::FindCodepoint(const char32_t codepoint) const
    {
        const auto glyph = FindGlyphId(codepoint);
        return glyph ? FindGlyph(*glyph) : nullptr;
    }

    std::optional<GlyphId> FontAtlas::FindGlyphId(const char32_t codepoint) const
    {
        const auto it = _codepointToGlyph.find(codepoint);
        if (it == _codepointToGlyph.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    const AtlasGlyph *FontAtlas::GetFallbackGlyph() const
    {
        return _fallback ? &_glyphs[*_fallback] : nullptr;
    }
}
