#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <text/FontAtlas.hpp>

#include "TextTestSupport.hpp"

using namespace N2Engine::Text;
using namespace TextTestSupport;

namespace
{
    class FontAtlasTest : public TextBackendTest
    {
    };

    std::uint64_t Fnv1a(const std::vector<std::uint8_t> &bytes)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const std::uint8_t byte : bytes)
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::vector<const AtlasGlyph *> GlyphsWithBitmaps(const FontAtlas &atlas)
    {
        std::vector<const AtlasGlyph *> result;
        for (const AtlasGlyph &glyph : atlas.GetGlyphs())
        {
            if (glyph.HasBitmap())
            {
                result.push_back(&glyph);
            }
        }
        return result;
    }
}

TEST(AtlasCharsetTest, Latin1IsPrintableAsciiPlusTheSupplement)
{
    const auto charset = ResolveCharset(AtlasSettings{});
    EXPECT_EQ(charset.size(), 95u + 96u);
    EXPECT_EQ(charset.front(), U' ');
    EXPECT_EQ(charset.back(), char32_t{0xFF});
    EXPECT_TRUE(std::ranges::is_sorted(charset));
    EXPECT_EQ(std::ranges::find(charset, char32_t{0x7F}), charset.end());
    EXPECT_EQ(std::ranges::find(charset, char32_t{0x9F}), charset.end());
}

TEST(AtlasCharsetTest, AsciiStopsAtTilde)
{
    AtlasSettings settings;
    settings.charset = Charset::Ascii;
    const auto charset = ResolveCharset(settings);
    EXPECT_EQ(charset.size(), 95u);
    EXPECT_EQ(charset.back(), U'~');
}

TEST(AtlasCharsetTest, ExtraCharsAreAddedOnceAndControlsAreDropped)
{
    AtlasSettings settings;
    settings.charset = Charset::Ascii;
    settings.extraChars = U"\u20AC\u20ACA\n\uFFFD";
    const auto charset = ResolveCharset(settings);
    EXPECT_EQ(charset.size(), 95u + 2u); // euro and U+FFFD; 'A' was already there; '\n' never draws
    EXPECT_EQ(charset.back(), char32_t{0xFFFD});
    EXPECT_TRUE(std::ranges::is_sorted(charset));
}

TEST_P(FontAtlasTest, EveryCharsetGlyphIsPresent)
{
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    for (const char32_t c : ResolveCharset(atlas.GetSettings()))
    {
        const AtlasGlyph *glyph = atlas.FindCodepoint(c);
        ASSERT_NE(glyph, nullptr) << "U+" << std::hex << static_cast<unsigned>(c);
        EXPECT_GT(glyph->advance, 0.0f) << static_cast<unsigned>(c);
        // Spaces have no outline; everything else visible has a bitmap
        if (c != U' ' && c != 0xA0)
        {
            EXPECT_TRUE(glyph->HasBitmap()) << "U+" << std::hex << static_cast<unsigned>(c);
        }
    }
    EXPECT_EQ(atlas.FindCodepoint(0x4E2D), nullptr);
    EXPECT_EQ(atlas.FindCodepoint(0xFFFD), nullptr); // in the font, but not in the default charset
}

TEST_P(FontAtlasTest, UsesTheDefaultSettings)
{
    const AtlasSettings &settings = DefaultFont().GetAtlas().GetSettings();
    EXPECT_EQ(settings.basePx, 48.0f);
    EXPECT_EQ(settings.spreadPx, 8);
    EXPECT_EQ(settings.charset, Charset::Latin1);
}

TEST_P(FontAtlasTest, GlyphRectsDontOverlapAndKeepThePadding)
{
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    const int padding = atlas.GetSettings().paddingPx;
    const auto glyphs = GlyphsWithBitmaps(atlas);
    ASSERT_GT(glyphs.size(), 150u);

    for (const AtlasGlyph *glyph : glyphs)
    {
        const PixelRect &r = glyph->pixels;
        EXPECT_GE(r.x, padding);
        EXPECT_GE(r.y, padding);
        EXPECT_LE(r.x + r.width + padding, atlas.GetWidth());
        EXPECT_LE(r.y + r.height + padding, atlas.GetHeight());
    }

    for (std::size_t i = 0; i < glyphs.size(); ++i)
    {
        for (std::size_t j = i + 1; j < glyphs.size(); ++j)
        {
            const PixelRect &a = glyphs[i]->pixels;
            const PixelRect &b = glyphs[j]->pixels;
            // Grow one rect by the padding: it still mustn't touch the other
            const bool separated = a.x + a.width + padding <= b.x || b.x + b.width + padding <= a.x ||
                                   a.y + a.height + padding <= b.y || b.y + b.height + padding <= a.y;
            EXPECT_TRUE(separated) << "glyphs " << glyphs[i]->glyph << " and " << glyphs[j]->glyph;
        }
    }
}

TEST_P(FontAtlasTest, UvsMatchThePixelRects)
{
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    const float width = static_cast<float>(atlas.GetWidth());
    const float height = static_cast<float>(atlas.GetHeight());
    for (const AtlasGlyph *glyph : GlyphsWithBitmaps(atlas))
    {
        const PixelRect &r = glyph->pixels;
        EXPECT_FLOAT_EQ(glyph->uv.minX, static_cast<float>(r.x) / width);
        EXPECT_FLOAT_EQ(glyph->uv.minY, static_cast<float>(r.y) / height);
        EXPECT_FLOAT_EQ(glyph->uv.maxX, static_cast<float>(r.x + r.width) / width);
        EXPECT_FLOAT_EQ(glyph->uv.maxY, static_cast<float>(r.y + r.height) / height);

        // The plane rect is the bitmap's size in ems
        const float basePx = atlas.GetSettings().basePx;
        EXPECT_NEAR(glyph->plane.Width(), static_cast<float>(r.width) / basePx, 1e-5f);
        EXPECT_NEAR(glyph->plane.Height(), static_cast<float>(r.height) / basePx, 1e-5f);
    }
}

TEST_P(FontAtlasTest, BitmapsAreCopiedIntoTheAtlas)
{
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    ASSERT_EQ(atlas.GetPixels().size(), static_cast<std::size_t>(atlas.GetWidth()) * atlas.GetHeight());

    // The atlas pixels of 'I' are exactly what the face renders at the atlas settings
    const AtlasGlyph *glyph = atlas.FindCodepoint(U'I');
    ASSERT_NE(glyph, nullptr);
    const AtlasSettings &settings = atlas.GetSettings();
    const GlyphSdf sdf = DefaultFont().GetFace().RenderSdf(glyph->glyph, settings.basePx, settings.spreadPx);
    ASSERT_EQ(sdf.width, glyph->pixels.width);
    ASSERT_EQ(sdf.height, glyph->pixels.height);
    for (int y = 0; y < sdf.height; ++y)
    {
        for (int x = 0; x < sdf.width; ++x)
        {
            const std::size_t atlasIndex = static_cast<std::size_t>(glyph->pixels.y + y) * atlas.GetWidth() + glyph->pixels.x + x;
            ASSERT_EQ(atlas.GetPixels()[atlasIndex], sdf.pixels[static_cast<std::size_t>(y) * sdf.width + x]);
        }
    }
}

TEST_P(FontAtlasTest, SizesArePowersOfTwo)
{
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    const auto isPowerOfTwo = [](const int v) { return v > 0 && (v & (v - 1)) == 0; };
    EXPECT_TRUE(isPowerOfTwo(atlas.GetWidth())) << atlas.GetWidth();
    EXPECT_TRUE(isPowerOfTwo(atlas.GetHeight())) << atlas.GetHeight();
    EXPECT_LE(atlas.GetWidth(), FontAtlas::kMaxSize);
    EXPECT_LE(atlas.GetHeight(), FontAtlas::kMaxSize);
}

TEST_P(FontAtlasTest, NotdefIsTheFallbackGlyph)
{
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    const AtlasGlyph *fallback = atlas.GetFallbackGlyph();
    ASSERT_NE(fallback, nullptr);
    EXPECT_EQ(fallback->glyph, 0u); // Noto's .notdef has an outline (a box)
    EXPECT_TRUE(fallback->HasBitmap());
    EXPECT_EQ(atlas.FindGlyph(0), fallback);
}

TEST_P(FontAtlasTest, BuildingTwiceGivesIdenticalAtlases)
{
    AtlasSettings settings = SmallSettings();
    settings.extraChars = U"\u00E9\u00F1";
    const auto first = MakeFont(GetParam(), settings);
    const auto second = MakeFont(GetParam(), settings);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const FontAtlas &a = first->GetAtlas();
    const FontAtlas &b = second->GetAtlas();
    EXPECT_EQ(a.GetWidth(), b.GetWidth());
    EXPECT_EQ(a.GetHeight(), b.GetHeight());
    EXPECT_EQ(Fnv1a(a.GetPixels()), Fnv1a(b.GetPixels()));
    EXPECT_EQ(a.GetPixels(), b.GetPixels());
    ASSERT_EQ(a.GetGlyphs().size(), b.GetGlyphs().size());
    for (std::size_t i = 0; i < a.GetGlyphs().size(); ++i)
    {
        EXPECT_EQ(a.GetGlyphs()[i].glyph, b.GetGlyphs()[i].glyph);
        EXPECT_EQ(a.GetGlyphs()[i].pixels, b.GetGlyphs()[i].pixels);
        EXPECT_EQ(a.GetGlyphs()[i].uv, b.GetGlyphs()[i].uv);
        EXPECT_EQ(a.GetGlyphs()[i].plane, b.GetGlyphs()[i].plane);
    }
}

TEST_P(FontAtlasTest, ExtraCharsExtendTheCharset)
{
    AtlasSettings settings = SmallSettings();
    settings.extraChars = U"\u00E9\uFFFD\u4E2D"; // the font has the first two only
    const auto font = MakeFont(GetParam(), settings);
    ASSERT_NE(font, nullptr);
    const FontAtlas &atlas = font->GetAtlas();
    EXPECT_NE(atlas.FindCodepoint(0xE9), nullptr);
    EXPECT_NE(atlas.FindCodepoint(0xFFFD), nullptr);
    EXPECT_EQ(atlas.FindCodepoint(0x4E2D), nullptr);
    EXPECT_EQ(atlas.FindCodepoint(0xF1), nullptr); // Latin-1, but the charset is ASCII
}

TEST_P(FontAtlasTest, PaddingSettingIsHonoured)
{
    AtlasSettings settings = SmallSettings();
    settings.paddingPx = 5;
    const auto font = MakeFont(GetParam(), settings);
    ASSERT_NE(font, nullptr);
    const FontAtlas &atlas = font->GetAtlas();
    const auto glyphs = GlyphsWithBitmaps(atlas);
    for (std::size_t i = 0; i < glyphs.size(); ++i)
    {
        const PixelRect &a = glyphs[i]->pixels;
        EXPECT_GE(a.x, 5);
        EXPECT_GE(a.y, 5);
        for (std::size_t j = i + 1; j < glyphs.size(); ++j)
        {
            const PixelRect &b = glyphs[j]->pixels;
            EXPECT_TRUE(a.x + a.width + 5 <= b.x || b.x + b.width + 5 <= a.x ||
                        a.y + a.height + 5 <= b.y || b.y + b.height + 5 <= a.y);
        }
    }
}

TEST_P(FontAtlasTest, InvalidSettingsAreRejected)
{
    const auto backend = CreateFontBackend(GetParam());
    const auto face = backend->LoadFace(GetDefaultFontData());
    ASSERT_NE(face, nullptr);

    const auto rejects = [&face](const AtlasSettings &settings)
    {
        std::string error;
        const bool rejected = !FontAtlas::Build(*face, settings, &error).has_value();
        return rejected && !error.empty();
    };

    AtlasSettings settings = SmallSettings();
    settings.basePx = 0.0f;
    EXPECT_TRUE(rejects(settings));
    settings = SmallSettings();
    settings.basePx = 10000.0f;
    EXPECT_TRUE(rejects(settings));
    settings = SmallSettings();
    settings.spreadPx = 0;
    EXPECT_TRUE(rejects(settings));
    settings = SmallSettings();
    settings.paddingPx = -1;
    EXPECT_TRUE(rejects(settings));
}

INSTANTIATE_TEST_SUITE_P(Backends, FontAtlasTest, ::testing::ValuesIn(Backends()), BackendName);
