#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "TextTestSupport.hpp"

using namespace N2Engine::Text;
using namespace TextTestSupport;

namespace
{
    class FontBackendTest : public TextBackendTest
    {
    protected:
        void SetUp() override
        {
            const auto backend = CreateFontBackend(GetParam());
            ASSERT_NE(backend, nullptr);
            std::string error;
            _face = backend->LoadFace(GetDefaultFontData(), &error);
            ASSERT_NE(_face, nullptr) << error;
        }

        GlyphId Glyph(const char32_t codepoint) const
        {
            const auto glyph = _face->FindGlyph(codepoint);
            EXPECT_TRUE(glyph.has_value()) << "no glyph for U+" << std::hex << static_cast<unsigned>(codepoint);
            return glyph.value_or(0);
        }

        std::unique_ptr<IFontFace> _face;
    };
}

TEST(FontBackendRegistryTest, TheDefaultBackendIsStbTrueTypeAndAvailable)
{
    EXPECT_EQ(GetDefaultFontBackendKind(), FontBackendKind::StbTrueType);
    ASSERT_FALSE(GetAvailableFontBackends().empty());
    EXPECT_EQ(GetAvailableFontBackends().front(), GetDefaultFontBackendKind());
    EXPECT_TRUE(IsFontBackendAvailable(FontBackendKind::StbTrueType));

    const auto backend = CreateDefaultFontBackend();
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->GetKind(), FontBackendKind::StbTrueType);
}

TEST(FontBackendRegistryTest, EveryListedBackendCanBeCreated)
{
    for (const FontBackendKind kind : GetAvailableFontBackends())
    {
        const auto backend = CreateFontBackend(kind);
        ASSERT_NE(backend, nullptr) << ToString(kind);
        EXPECT_EQ(backend->GetKind(), kind);
    }
}

TEST(FontBackendRegistryTest, TheEmbeddedDefaultFontIsATrueTypeFile)
{
    const auto data = GetDefaultFontData();
    ASSERT_GT(data.size(), 1000u);
    // sfnt version 1.0: a TrueType outline font
    EXPECT_EQ(data[0], std::byte{0x00});
    EXPECT_EQ(data[1], std::byte{0x01});
    EXPECT_EQ(data[2], std::byte{0x00});
    EXPECT_EQ(data[3], std::byte{0x00});
}

TEST_P(FontBackendTest, MetricsAreSane)
{
    const FontMetrics metrics = _face->GetMetrics();
    EXPECT_GT(metrics.unitsPerEm, 0);
    EXPECT_GT(metrics.ascent, 0);
    EXPECT_LT(metrics.descent, 0);
    EXPECT_GE(metrics.lineGap, 0);
    EXPECT_GT(_face->GetGlyphCount(), 190u); // ASCII + Latin-1 + .notdef
}

TEST_P(FontBackendTest, FindsGlyphsForTheSubsetAndNothingElse)
{
    for (char32_t c = 0x20; c <= 0x7E; ++c)
    {
        EXPECT_TRUE(_face->FindGlyph(c).has_value()) << static_cast<unsigned>(c);
    }
    for (char32_t c = 0xA0; c <= 0xFF; ++c)
    {
        EXPECT_TRUE(_face->FindGlyph(c).has_value()) << static_cast<unsigned>(c);
    }
    EXPECT_TRUE(_face->FindGlyph(0xFFFD).has_value());

    EXPECT_FALSE(_face->FindGlyph(0x4E2D).has_value()); // CJK, not in the subset
    EXPECT_FALSE(_face->FindGlyph(0x110000).has_value());
    EXPECT_FALSE(_face->FindGlyph(0).has_value()); // never .notdef
}

TEST_P(FontBackendTest, WideGlyphsAdvanceFurtherThanNarrowOnes)
{
    const GlyphMetrics w = _face->GetGlyphMetrics(Glyph(U'W'));
    const GlyphMetrics i = _face->GetGlyphMetrics(Glyph(U'i'));
    EXPECT_GT(w.advance, i.advance);
    EXPECT_GT(i.advance, 0);
    EXPECT_GT(_face->GetGlyphMetrics(Glyph(U' ')).advance, 0);
}

TEST_P(FontBackendTest, KerningPairsTightenAndUnrelatedPairsDont)
{
    // Noto Sans kerns A/V and T/o (its 'kern' table: README in engine/assets/fonts)
    EXPECT_LT(_face->GetKerning(Glyph(U'A'), Glyph(U'V')), 0);
    EXPECT_LT(_face->GetKerning(Glyph(U'T'), Glyph(U'o')), 0);
    EXPECT_EQ(_face->GetKerning(Glyph(U'l'), Glyph(U'l')), 0);
}

TEST_P(FontBackendTest, InvalidGlyphIdsAreHarmless)
{
    const GlyphId invalid = _face->GetGlyphCount() + 10;
    EXPECT_EQ(_face->GetGlyphMetrics(invalid).advance, 0);
    EXPECT_EQ(_face->GetKerning(invalid, Glyph(U'A')), 0);
    EXPECT_EQ(_face->RenderSdf(invalid, 32.0f, 4).width, 0);
}

TEST_P(FontBackendTest, SdfHasTheEdgeAtMidValueAndTheSpreadAsBorder)
{
    const int spread = 6;
    const GlyphSdf sdf = _face->RenderSdf(Glyph(U'I'), 48.0f, spread);
    ASSERT_GT(sdf.width, 2 * spread);
    ASSERT_GT(sdf.height, 2 * spread);
    ASSERT_EQ(sdf.pixels.size(), static_cast<std::size_t>(sdf.width) * sdf.height);

    // The border is at least spread pixels from the outline, so it's fully outside
    const auto at = [&sdf](const int x, const int y) { return sdf.pixels[static_cast<std::size_t>(y) * sdf.width + x]; };
    EXPECT_EQ(at(0, 0), 0);
    EXPECT_EQ(at(sdf.width - 1, sdf.height - 1), 0);
    // The centre of an 'I' stem is inside
    EXPECT_GT(at(sdf.width / 2, sdf.height / 2), 128);

    // The bitmap sits above the baseline, offset by the spread
    EXPECT_LT(sdf.yOffset, 0);
    EXPECT_LE(sdf.yOffset + sdf.height, spread + 1);
}

TEST_P(FontBackendTest, SpacesHaveNoSdf)
{
    const GlyphSdf sdf = _face->RenderSdf(Glyph(U' '), 48.0f, 8);
    EXPECT_EQ(sdf.width, 0);
    EXPECT_EQ(sdf.height, 0);
    EXPECT_TRUE(sdf.pixels.empty());
}

TEST_P(FontBackendTest, BadParametersRenderNothing)
{
    EXPECT_EQ(_face->RenderSdf(Glyph(U'A'), 0.0f, 8).width, 0);
    EXPECT_EQ(_face->RenderSdf(Glyph(U'A'), 48.0f, 0).width, 0);
}

TEST_P(FontBackendTest, RejectsDataThatIsNotAFont)
{
    const auto backend = CreateFontBackend(GetParam());
    std::string error;

    EXPECT_EQ(backend->LoadFace({}, &error), nullptr);
    EXPECT_FALSE(error.empty());

    std::vector<std::byte> garbage(256, std::byte{0x5A});
    error.clear();
    EXPECT_EQ(backend->LoadFace(garbage, &error), nullptr);
    EXPECT_FALSE(error.empty());

    // A real font cut short: its table directory points past the end
    const auto font = GetDefaultFontData();
    const std::vector<std::byte> truncated(font.begin(), font.begin() + 200);
    error.clear();
    EXPECT_EQ(backend->LoadFace(truncated, &error), nullptr);
    EXPECT_FALSE(error.empty());

    EXPECT_EQ(backend->LoadFace(truncated), nullptr); // no error out-parameter is fine too
}

TEST_P(FontBackendTest, TheFaceOwnsItsCopyOfTheData)
{
    const auto backend = CreateFontBackend(GetParam());
    std::unique_ptr<IFontFace> face;
    {
        const auto font = GetDefaultFontData();
        std::vector<std::byte> copy(font.begin(), font.end());
        face = backend->LoadFace(copy);
        std::ranges::fill(copy, std::byte{0}); // scribble over the caller's buffer
    }
    ASSERT_NE(face, nullptr);
    EXPECT_GT(face->GetMetrics().ascent, 0);
    EXPECT_GT(face->RenderSdf(*face->FindGlyph(U'A'), 24.0f, 4).width, 0);
}

INSTANTIATE_TEST_SUITE_P(Backends, FontBackendTest, ::testing::ValuesIn(Backends()), BackendName);
