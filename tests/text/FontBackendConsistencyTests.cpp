#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <text/DefaultFont.hpp>
#include <text/FontAtlas.hpp>
#include <text/FontBackend.hpp>

// Every backend in the build against stb_truetype (the default) on the bundled font. Layout and atlases
// must not depend on which backend loaded a font, so everything in font units is identical, and SDF
// bitmaps have the same size and placement with closely matching values (each library computes the
// distances its own way, so exact pixel equality isn't expected). Skipped when the build has one backend.
using namespace N2Engine::Text;

namespace
{
    std::vector<FontBackendKind> OtherBackends()
    {
        std::vector<FontBackendKind> result;
        for (const FontBackendKind kind : GetAvailableFontBackends())
        {
            if (kind != FontBackendKind::StbTrueType)
            {
                result.push_back(kind);
            }
        }
        return result;
    }

    std::vector<char32_t> TestCodepoints()
    {
        std::vector<char32_t> codepoints;
        for (char32_t c = 0x20; c <= 0x7E; ++c)
            codepoints.push_back(c);
        for (char32_t c = 0xA0; c <= 0xFF; ++c)
            codepoints.push_back(c);
        codepoints.push_back(0xFFFD);
        return codepoints;
    }

    class FontBackendConsistencyTest : public ::testing::TestWithParam<FontBackendKind>
    {
    protected:
        void SetUp() override
        {
            const auto reference = CreateFontBackend(FontBackendKind::StbTrueType);
            const auto other = CreateFontBackend(GetParam());
            ASSERT_NE(reference, nullptr);
            ASSERT_NE(other, nullptr);
            std::string error;
            _stb = reference->LoadFace(GetDefaultFontData(), &error);
            ASSERT_NE(_stb, nullptr) << error;
            _other = other->LoadFace(GetDefaultFontData(), &error);
            ASSERT_NE(_other, nullptr) << error;
        }

        std::unique_ptr<IFontFace> _stb;
        std::unique_ptr<IFontFace> _other;
    };

    std::string Describe(const GlyphId glyph, const float pixelsPerEm, const int spreadPx)
    {
        return "glyph " + std::to_string(glyph) + " at " + std::to_string(pixelsPerEm) + " px, spread " +
               std::to_string(spreadPx);
    }
}

TEST_P(FontBackendConsistencyTest, FontMetricsMatch)
{
    const FontMetrics a = _stb->GetMetrics();
    const FontMetrics b = _other->GetMetrics();
    EXPECT_EQ(a.ascent, b.ascent);
    EXPECT_EQ(a.descent, b.descent);
    EXPECT_EQ(a.lineGap, b.lineGap);
    EXPECT_EQ(a.unitsPerEm, b.unitsPerEm);
    EXPECT_EQ(_stb->GetGlyphCount(), _other->GetGlyphCount());
}

TEST_P(FontBackendConsistencyTest, CodepointsMapToTheSameGlyphs)
{
    for (const char32_t c : TestCodepoints())
    {
        EXPECT_EQ(_stb->FindGlyph(c), _other->FindGlyph(c)) << "U+" << std::hex << static_cast<unsigned>(c);
    }
    for (const char32_t c : {char32_t{0}, char32_t{0x4E2D}, char32_t{0x10FFFF}, char32_t{0x110000}})
    {
        EXPECT_EQ(_stb->FindGlyph(c), _other->FindGlyph(c)) << "U+" << std::hex << static_cast<unsigned>(c);
    }
}

TEST_P(FontBackendConsistencyTest, EveryGlyphHasTheSameMetricsAndBounds)
{
    // Every glyph id, plus one past the end
    for (GlyphId glyph = 0; glyph <= _stb->GetGlyphCount(); ++glyph)
    {
        const GlyphMetrics a = _stb->GetGlyphMetrics(glyph);
        const GlyphMetrics b = _other->GetGlyphMetrics(glyph);
        EXPECT_EQ(a.advance, b.advance) << "glyph " << glyph;
        EXPECT_EQ(a.leftSideBearing, b.leftSideBearing) << "glyph " << glyph;

        const auto boundsA = _stb->GetGlyphBounds(glyph);
        const auto boundsB = _other->GetGlyphBounds(glyph);
        ASSERT_EQ(boundsA.has_value(), boundsB.has_value()) << "glyph " << glyph;
        if (boundsA)
        {
            EXPECT_EQ(boundsA->minX, boundsB->minX) << "glyph " << glyph;
            EXPECT_EQ(boundsA->minY, boundsB->minY) << "glyph " << glyph;
            EXPECT_EQ(boundsA->maxX, boundsB->maxX) << "glyph " << glyph;
            EXPECT_EQ(boundsA->maxY, boundsB->maxY) << "glyph " << glyph;
        }
    }
}

TEST_P(FontBackendConsistencyTest, KerningMatchesForEveryAsciiPair)
{
    // The bundled font has a legacy 'kern' table, which both libraries read
    std::vector<GlyphId> glyphs;
    for (char32_t c = 0x20; c <= 0x7E; ++c)
    {
        if (const auto glyph = _stb->FindGlyph(c))
            glyphs.push_back(*glyph);
    }
    int kernedPairs = 0;
    for (const GlyphId left : glyphs)
    {
        for (const GlyphId right : glyphs)
        {
            const int kerning = _stb->GetKerning(left, right);
            EXPECT_EQ(kerning, _other->GetKerning(left, right)) << "glyphs " << left << " and " << right;
            kernedPairs += kerning != 0 ? 1 : 0;
        }
    }
    EXPECT_GT(kernedPairs, 100); // the comparison covered real kerning, not just zeros
}

TEST_P(FontBackendConsistencyTest, SdfBitmapsHaveTheSamePlacementAndSimilarValues)
{
    struct Size
    {
        float pixelsPerEm;
        int spreadPx;
    };
    // The default atlas size, the small test size, a fractional size, and the smallest spread (which
    // FreeType renders at 2 and rescales)
    const Size sizes[] = {{48.0f, 8}, {16.0f, 4}, {22.5f, 3}, {12.0f, 1}};

    long long totalPixels = 0;
    long long sideMismatches = 0;
    for (const Size size : sizes)
    {
        for (const char32_t c : TestCodepoints())
        {
            const auto glyph = _stb->FindGlyph(c);
            ASSERT_TRUE(glyph.has_value());
            const GlyphSdf a = _stb->RenderSdf(*glyph, size.pixelsPerEm, size.spreadPx);
            const GlyphSdf b = _other->RenderSdf(*glyph, size.pixelsPerEm, size.spreadPx);
            const std::string what = Describe(*glyph, size.pixelsPerEm, size.spreadPx);

            ASSERT_EQ(a.width, b.width) << what;
            ASSERT_EQ(a.height, b.height) << what;
            EXPECT_EQ(a.xOffset, b.xOffset) << what;
            EXPECT_EQ(a.yOffset, b.yOffset) << what;
            ASSERT_EQ(a.pixels.size(), b.pixels.size()) << what;
            if (a.pixels.empty())
            {
                continue;
            }

            long long insideA = 0;
            long long insideB = 0;
            long long differenceSum = 0;
            for (std::size_t i = 0; i < a.pixels.size(); ++i)
            {
                // Inside means above 128, not 128 or above. A pixel less than spreadPx / 128 of a pixel from
                // the outline is exactly 128 in FreeType on either side (it truncates the distance towards
                // zero), while stb_truetype rounds an outside one down to 127. On an axis-aligned stem that
                // edge pixel is a whole row or column, so counting 128 as inside would charge FreeType with
                // dozens of extra inside pixels for a difference of under 1/16 of a pixel.
                const bool inA = a.pixels[i] > 128;
                const bool inB = b.pixels[i] > 128;
                insideA += inA ? 1 : 0;
                insideB += inB ? 1 : 0;
                sideMismatches += inA != inB ? 1 : 0;
                differenceSum += std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i]));
            }
            totalPixels += static_cast<long long>(a.pixels.size());

            // The same coverage, give or take a pixel along the edge here and there
            EXPECT_LE(std::llabs(insideA - insideB), 4 + insideA / 20) << what << ": " << insideA << " vs " << insideB;
            // Close distance values: a few levels on average (one pixel of distance is 128 / spreadPx levels,
            // and the libraries round and resolve corners differently)
            const double meanDifference = static_cast<double>(differenceSum) / static_cast<double>(a.pixels.size());
            EXPECT_LE(meanDifference, 8.0) << what;
        }
    }
    ASSERT_GT(totalPixels, 0);
    // Across every glyph and size, under 2% of pixels land on opposite sides of the edge
    EXPECT_LT(static_cast<double>(sideMismatches) / static_cast<double>(totalPixels), 0.02)
        << sideMismatches << " of " << totalPixels;
}

TEST_P(FontBackendConsistencyTest, AtlasesHaveTheSameLayout)
{
    // Same glyph sizes and placement in both backends, so the packer produces the same atlas layout
    AtlasSettings settings;
    settings.basePx = 16.0f;
    settings.spreadPx = 4;
    settings.charset = Charset::Ascii;
    std::string error;
    const auto a = FontAtlas::Build(*_stb, settings, &error);
    ASSERT_TRUE(a.has_value()) << error;
    const auto b = FontAtlas::Build(*_other, settings, &error);
    ASSERT_TRUE(b.has_value()) << error;

    EXPECT_EQ(a->GetWidth(), b->GetWidth());
    EXPECT_EQ(a->GetHeight(), b->GetHeight());
    ASSERT_EQ(a->GetGlyphs().size(), b->GetGlyphs().size());
    for (std::size_t i = 0; i < a->GetGlyphs().size(); ++i)
    {
        const AtlasGlyph &ga = a->GetGlyphs()[i];
        const AtlasGlyph &gb = b->GetGlyphs()[i];
        EXPECT_EQ(ga.glyph, gb.glyph);
        EXPECT_EQ(ga.pixels, gb.pixels) << "glyph " << ga.glyph;
        EXPECT_EQ(ga.plane, gb.plane) << "glyph " << ga.glyph;
        EXPECT_EQ(ga.advance, gb.advance) << "glyph " << ga.glyph;
    }
}

// Instantiated with no values when stb_truetype is the only backend in the build
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(FontBackendConsistencyTest);
INSTANTIATE_TEST_SUITE_P(Backends, FontBackendConsistencyTest, ::testing::ValuesIn(OtherBackends()),
                         [](const ::testing::TestParamInfo<FontBackendKind> &info)
                         {
                             return std::string(ToString(info.param));
                         });
