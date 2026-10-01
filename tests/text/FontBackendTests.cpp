#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
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
        // Only a face: these tests don't need the shared default font's atlas
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

    // A copy of the default font with a table directory to find and patch tables by tag
    class FontBytes
    {
    public:
        FontBytes()
        {
            const auto font = GetDefaultFontData();
            _bytes.assign(font.begin(), font.end());
        }

        struct Table
        {
            std::size_t record = 0; // offset of the directory record
            std::size_t offset = 0;
            std::size_t length = 0;
        };

        Table Find(const char (&tag)[5]) const
        {
            const std::size_t count = U16(4);
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::size_t record = 12 + i * 16;
                if (std::memcmp(_bytes.data() + record, tag, 4) == 0)
                {
                    return {record, U32(record + 8), U32(record + 12)};
                }
            }
            ADD_FAILURE() << "no " << tag << " table";
            return {};
        }

        std::size_t U16(const std::size_t at) const
        {
            return (std::to_integer<std::size_t>(_bytes[at]) << 8) | std::to_integer<std::size_t>(_bytes[at + 1]);
        }
        std::size_t U32(const std::size_t at) const { return (U16(at) << 16) | U16(at + 2); }

        void SetU16(const std::size_t at, const std::size_t value)
        {
            _bytes[at] = static_cast<std::byte>((value >> 8) & 0xFF);
            _bytes[at + 1] = static_cast<std::byte>(value & 0xFF);
        }
        void SetU32(const std::size_t at, const std::size_t value)
        {
            SetU16(at, (value >> 16) & 0xFFFF);
            SetU16(at + 2, value & 0xFFFF);
        }
        /// Makes the directory say the table is this long (it still lies inside the file)
        void SetLength(const char (&tag)[5], const std::size_t length) { SetU32(Find(tag).record + 12, length); }

        [[nodiscard]] const std::vector<std::byte> &Bytes() const { return _bytes; }

    private:
        std::vector<std::byte> _bytes;
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

TEST_P(FontBackendTest, CorruptTablesAreRejected)
{
    const auto backend = CreateFontBackend(GetParam());
    const auto rejects = [&backend](const FontBytes &font, const char *what)
    {
        std::string error;
        const bool rejected = backend->LoadFace(font.Bytes(), &error) == nullptr;
        EXPECT_TRUE(rejected) << what;
        EXPECT_FALSE(error.empty()) << what;
    };

    // The unmodified copy loads, so each failure below is the patch's doing
    ASSERT_NE(backend->LoadFace(FontBytes().Bytes()), nullptr);

    const FontBytes original;
    const std::size_t glyphCount = original.U16(original.Find("maxp").offset + 4);
    const std::size_t locaFormat = original.U16(original.Find("head").offset + 50);
    const std::size_t locaEntry = locaFormat == 0 ? 2 : 4;

    FontBytes font;
    font.SetLength("head", 20);
    rejects(font, "head shorter than 54 bytes");

    font = FontBytes();
    font.SetU16(font.Find("head").offset + 50, 2);
    rejects(font, "indexToLocFormat 2");

    font = FontBytes();
    font.SetU16(font.Find("maxp").offset + 4, 0);
    rejects(font, "no glyphs");

    font = FontBytes();
    font.SetU16(font.Find("hhea").offset + 34, 0);
    rejects(font, "numberOfHMetrics 0");

    font = FontBytes();
    font.SetU16(font.Find("hhea").offset + 34, glyphCount + 1);
    rejects(font, "numberOfHMetrics above numGlyphs");

    font = FontBytes();
    font.SetLength("hmtx", 4);
    rejects(font, "hmtx too short");

    font = FontBytes();
    font.SetLength("loca", locaEntry * glyphCount); // one entry short
    rejects(font, "loca too short");

    // The last loca entry pointing past the end of glyf
    font = FontBytes();
    const auto loca = font.Find("loca");
    const std::size_t last = loca.offset + glyphCount * locaEntry;
    if (locaEntry == 2)
        font.SetU16(last, 0xFFFF);
    else
        font.SetU32(last, 0xFFFFFFFF);
    rejects(font, "loca past glyf");

    // An entry larger than the next one: glyph 1 claims to start at the end of glyf
    font = FontBytes();
    const std::size_t glyfLength = font.Find("glyf").length;
    if (locaEntry == 2)
        font.SetU16(loca.offset + 2, glyfLength / 2);
    else
        font.SetU32(loca.offset + 4, glyfLength);
    rejects(font, "loca out of order");
}

TEST_P(FontBackendTest, GlyphBoundsCoverTheOutline)
{
    const auto bounds = _face->GetGlyphBounds(Glyph(U'I'));
    ASSERT_TRUE(bounds.has_value());
    EXPECT_FALSE(bounds->IsEmpty());
    EXPECT_GE(bounds->minY, 0.0f); // 'I' sits on the baseline
    EXPECT_LE(bounds->maxX, static_cast<float>(_face->GetGlyphMetrics(Glyph(U'I')).advance));

    EXPECT_FALSE(_face->GetGlyphBounds(Glyph(U' ')).has_value());
    EXPECT_FALSE(_face->GetGlyphBounds(_face->GetGlyphCount() + 1).has_value());
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
