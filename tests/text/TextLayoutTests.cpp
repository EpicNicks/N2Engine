#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <text/Shaper.hpp>
#include <text/Utf8.hpp>
#include <text/TextLayout.hpp>

#include "TextTestSupport.hpp"

using namespace N2Engine::Text;
using namespace TextTestSupport;

namespace
{
    constexpr float kEpsilon = 1e-4f;

    class TextLayoutTest : public TextBackendTest
    {
    protected:
        TextLayout Layout(const std::string &text, const LayoutOptions &options = {}) const
        {
            return LayoutText(DefaultFont(), text, options);
        }

        /// Width of a single line laid out without wrapping
        float Width(const std::string &text, const LayoutOptions &options = {}) const
        {
            LayoutOptions unwrapped = options;
            unwrapped.maxWidth = 0.0f;
            const TextLayout layout = Layout(text, unwrapped);
            EXPECT_EQ(layout.lines.size(), 1u) << text;
            return layout.lines.empty() ? 0.0f : layout.lines[0].width;
        }

        float Advance(const char32_t codepoint) const
        {
            const AtlasGlyph *glyph = DefaultFont().GetAtlas().FindCodepoint(codepoint);
            EXPECT_NE(glyph, nullptr);
            return glyph != nullptr ? glyph->advance : 0.0f;
        }

        float Scale(const float fontSize = 1.0f) const
        {
            return fontSize / static_cast<float>(DefaultFont().GetMetrics().unitsPerEm);
        }
    };

    /// A shaper that maps every codepoint to 'x', to show layout goes through the shaper seam
    class EverythingIsXShaper final : public IShaper
    {
    public:
        void Shape(const SdfFont &font, const std::u32string_view codepoints, const std::span<const std::uint32_t> clusters,
                   std::vector<ShapedGlyph> &out) const override
        {
            const AtlasGlyph *x = font.GetAtlas().FindCodepoint(U'x');
            for (std::size_t i = 0; i < codepoints.size(); ++i)
            {
                ShapedGlyph glyph;
                glyph.glyph = x->glyph;
                glyph.codepoint = codepoints[i];
                glyph.cluster = clusters[i];
                glyph.xAdvance = x->advance;
                out.push_back(glyph);
            }
        }
    };
}

TEST_P(TextLayoutTest, EmptyTextHasNoLinesOrQuads)
{
    const TextLayout layout = Layout("");
    EXPECT_TRUE(layout.quads.empty());
    EXPECT_TRUE(layout.lines.empty());
    EXPECT_TRUE(layout.missingCodepoints.empty());
    EXPECT_EQ(layout.bounds, Rect{});
}

TEST_P(TextLayoutTest, OneLineAddsUpTheAdvances)
{
    const TextLayout layout = Layout("Hello");
    ASSERT_EQ(layout.lines.size(), 1u);
    ASSERT_EQ(layout.quads.size(), 5u);
    const float expected = Advance(U'H') + Advance(U'e') + 2.0f * Advance(U'l') + Advance(U'o');
    EXPECT_NEAR(layout.lines[0].width, expected, kEpsilon);

    // Quads run left to right, and each carries its codepoint and byte offset
    const std::u32string codepoints = U"Hello";
    for (std::size_t i = 0; i < layout.quads.size(); ++i)
    {
        EXPECT_EQ(layout.quads[i].codepoint, codepoints[i]);
        EXPECT_EQ(layout.quads[i].cluster, i);
        EXPECT_FALSE(layout.quads[i].position.IsEmpty());
        EXPECT_GE(layout.quads[i].uv.minX, 0.0f);
        EXPECT_LE(layout.quads[i].uv.maxX, 1.0f);
        if (i > 0)
        {
            EXPECT_GT(layout.quads[i].position.minX, layout.quads[i - 1].position.minX);
        }
    }
}

TEST_P(TextLayoutTest, QuadsUseTheAtlasGeometry)
{
    LayoutOptions options;
    options.fontSize = 2.0f;
    const TextLayout layout = Layout("I", options);
    ASSERT_EQ(layout.quads.size(), 1u);
    const AtlasGlyph *glyph = DefaultFont().GetAtlas().FindCodepoint(U'I');
    ASSERT_NE(glyph, nullptr);
    const GlyphQuad &quad = layout.quads[0];
    EXPECT_EQ(quad.uv, glyph->uv);

    const float baseline = layout.lines[0].baseline;
    EXPECT_NEAR(quad.position.minX, glyph->plane.minX * 2.0f, kEpsilon);
    EXPECT_NEAR(quad.position.maxX, glyph->plane.maxX * 2.0f, kEpsilon);
    EXPECT_NEAR(quad.position.minY, baseline + glyph->plane.minY * 2.0f, kEpsilon);
    EXPECT_NEAR(quad.position.maxY, baseline + glyph->plane.maxY * 2.0f, kEpsilon);
}

TEST_P(TextLayoutTest, FontSizeScalesEverything)
{
    LayoutOptions big;
    big.fontSize = 3.0f;
    EXPECT_NEAR(Width("Hello", big), 3.0f * Width("Hello"), kEpsilon);

    const TextLayout unit = Layout("Hi\nthere");
    const TextLayout scaled = Layout("Hi\nthere", big);
    EXPECT_NEAR(scaled.lineHeight, 3.0f * unit.lineHeight, kEpsilon);
    EXPECT_NEAR(scaled.bounds.Height(), 3.0f * unit.bounds.Height(), kEpsilon);
}

TEST_P(TextLayoutTest, KerningPullsPairsTogether)
{
    // "AV" is narrower than its two advances (the font kerns the pair); "ll" isn't kerned
    EXPECT_LT(Width("AV"), Advance(U'A') + Advance(U'V') - kEpsilon);
    EXPECT_NEAR(Width("ll"), 2.0f * Advance(U'l'), kEpsilon);
}

TEST_P(TextLayoutTest, LetterSpacingIsAddedBetweenGlyphs)
{
    LayoutOptions spaced;
    spaced.letterSpacing = 0.25f;
    spaced.fontSize = 2.0f;
    LayoutOptions plain;
    plain.fontSize = 2.0f;
    // Three gaps in four glyphs, in ems times the font size
    EXPECT_NEAR(Width("abcd", spaced), Width("abcd", plain) + 3.0f * 0.25f * 2.0f, kEpsilon);
}

TEST_P(TextLayoutTest, LineHeightComesFromTheFontMetrics)
{
    const FontMetrics &metrics = DefaultFont().GetMetrics();
    LayoutOptions options;
    options.lineSpacing = 1.5f;
    const TextLayout layout = Layout("a\nb", options);
    const float expected = static_cast<float>(metrics.ascent - metrics.descent + metrics.lineGap) * Scale() * 1.5f;
    EXPECT_NEAR(layout.lineHeight, expected, kEpsilon);
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_NEAR(layout.lines[0].baseline - layout.lines[1].baseline, expected, kEpsilon);
}

TEST_P(TextLayoutTest, NewlinesBreakLines)
{
    const TextLayout layout = Layout("ab\ncd");
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.quads.size(), 4u);
    EXPECT_EQ(layout.lines[0].quadCount, 2u);
    EXPECT_EQ(layout.lines[1].firstQuad, 2u);
    EXPECT_EQ(layout.lines[0].byteBegin, 0u);
    EXPECT_EQ(layout.lines[0].byteEnd, 2u);
    EXPECT_EQ(layout.lines[1].byteBegin, 3u);
    EXPECT_EQ(layout.lines[1].byteEnd, 5u);
    EXPECT_EQ(layout.quads[2].cluster, 3u);
    // Both lines start at the left edge, one line height apart
    const FontAtlas &atlas = DefaultFont().GetAtlas();
    EXPECT_NEAR(layout.quads[0].position.minX, atlas.FindCodepoint(U'a')->plane.minX, kEpsilon);
    EXPECT_NEAR(layout.quads[2].position.minX, atlas.FindCodepoint(U'c')->plane.minX, kEpsilon);
    EXPECT_NEAR(layout.lines[0].baseline - layout.lines[1].baseline, layout.lineHeight, kEpsilon);
}

TEST_P(TextLayoutTest, CrLfIsOneLineBreak)
{
    const TextLayout layout = Layout("ab\r\ncd");
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.quads.size(), 4u);
    EXPECT_EQ(layout.lines[1].byteBegin, 4u);
    EXPECT_NEAR(layout.lines[0].width, Width("ab"), kEpsilon);
}

TEST_P(TextLayoutTest, TrailingAndRepeatedNewlinesMakeEmptyLines)
{
    EXPECT_EQ(Layout("ab\n").lines.size(), 2u);
    const TextLayout layout = Layout("\n\n");
    ASSERT_EQ(layout.lines.size(), 3u);
    EXPECT_TRUE(layout.quads.empty());
    for (const LineInfo &line : layout.lines)
    {
        EXPECT_EQ(line.width, 0.0f);
    }
    EXPECT_GT(layout.bounds.Height(), 0.0f);
}

TEST_P(TextLayoutTest, OtherControlCharactersAreIgnored)
{
    const TextLayout layout = Layout("a\x01" "b\rc");
    ASSERT_EQ(layout.lines.size(), 1u);
    EXPECT_EQ(layout.quads.size(), 3u);
    EXPECT_TRUE(layout.missingCodepoints.empty());
}

TEST_P(TextLayoutTest, WrapsAtTheLastSpaceThatFits)
{
    LayoutOptions options;
    options.maxWidth = Width("aaa bbb") * 1.01f;
    const TextLayout layout = Layout("aaa bbb ccc", options);
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_NEAR(layout.lines[0].width, Width("aaa bbb"), kEpsilon);
    EXPECT_NEAR(layout.lines[1].width, Width("ccc"), kEpsilon);
    EXPECT_EQ(layout.lines[0].quadCount, 6u);
    EXPECT_EQ(layout.lines[1].byteBegin, 8u);
    EXPECT_EQ(layout.quads.size(), 9u);
    for (const LineInfo &line : layout.lines)
    {
        EXPECT_LE(line.width, options.maxWidth);
    }
}

TEST_P(TextLayoutTest, WrapsAtTabs)
{
    LayoutOptions options;
    options.maxWidth = Width("aaa") * 1.5f;
    const TextLayout layout = Layout("aaa\tbbb", options);
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.lines[1].byteBegin, 4u);
}

TEST_P(TextLayoutTest, TrailingSpacesDontCountOrWrap)
{
    EXPECT_NEAR(Width("abc   "), Width("abc"), kEpsilon);

    // Spaces past the edge stay on the line instead of starting a new one
    LayoutOptions options;
    options.maxWidth = Width("abc") * 1.01f;
    const TextLayout layout = Layout("abc      ", options);
    ASSERT_EQ(layout.lines.size(), 1u);
    EXPECT_NEAR(layout.lines[0].width, Width("abc"), kEpsilon);

    // and the spaces at a wrap point end the first line
    const TextLayout wrapped = Layout("abc    def", options);
    ASSERT_EQ(wrapped.lines.size(), 2u);
    EXPECT_EQ(wrapped.lines[1].byteBegin, 7u);
    EXPECT_EQ(wrapped.lines[0].byteEnd, 7u);
}

TEST_P(TextLayoutTest, LeadingSpacesCount)
{
    EXPECT_NEAR(Width("  a"), 2.0f * Advance(U' ') + Advance(U'a'), kEpsilon);
}

TEST_P(TextLayoutTest, LongWordsBreakMidWord)
{
    LayoutOptions options;
    options.maxWidth = 3.5f * Advance(U'W');
    const TextLayout layout = Layout("WWWWWWWWWW", options);
    EXPECT_EQ(layout.quads.size(), 10u);
    ASSERT_EQ(layout.lines.size(), 4u); // 3 + 3 + 3 + 1
    for (const LineInfo &line : layout.lines)
    {
        EXPECT_GT(line.quadCount, 0u);
        EXPECT_LE(line.width, options.maxWidth);
    }
    EXPECT_EQ(layout.lines[3].quadCount, 1u);
}

TEST_P(TextLayoutTest, AGlyphWiderThanTheLineStillGetsOne)
{
    LayoutOptions options;
    options.maxWidth = 0.1f * Advance(U'W');
    const TextLayout layout = Layout("WW", options);
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.lines[0].quadCount, 1u);
    EXPECT_EQ(layout.lines[1].quadCount, 1u);
}

TEST_P(TextLayoutTest, WrapsAfterHyphens)
{
    LayoutOptions options;
    options.maxWidth = Width("well-kn") * 1.01f;
    const TextLayout layout = Layout("well-known", options);
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_EQ(layout.lines[0].byteEnd, 5u); // "well-"
    EXPECT_EQ(layout.lines[1].byteBegin, 5u);
    EXPECT_NEAR(layout.lines[0].width, Width("well-"), kEpsilon);
}

TEST_P(TextLayoutTest, NoBreakSpaceDoesNotWrap)
{
    // Room for "aaa", U+00A0 and one 'b': a breaking space would wrap after the U+00A0 (byte 5), but
    // there's no break opportunity, so the word breaks before the second 'b' (byte 6) instead
    LayoutOptions options;
    options.maxWidth = Width("aaa\xC2\xA0" "b") * 1.01f;
    const TextLayout layout = Layout("aaa\xC2\xA0" "bbb", options);
    ASSERT_GE(layout.lines.size(), 2u);
    EXPECT_EQ(layout.lines[1].byteBegin, 6u);
}

TEST_P(TextLayoutTest, TabsAdvanceToTheNextStop)
{
    LayoutOptions options;
    options.tabWidth = 4.0f;
    const TextLayout layout = Layout("a\tb", options);
    ASSERT_EQ(layout.quads.size(), 2u);
    const float stop = 4.0f * Advance(U' ');
    const AtlasGlyph *b = DefaultFont().GetAtlas().FindCodepoint(U'b');
    EXPECT_NEAR(layout.quads[1].position.minX, stop + b->plane.minX, kEpsilon);
}

TEST_P(TextLayoutTest, HorizontalAlignmentIsAroundTheOrigin)
{
    LayoutOptions options;
    const float width = Width("Hello");

    options.horizontalAlign = HorizontalAlign::Left;
    TextLayout layout = Layout("Hello", options);
    EXPECT_NEAR(layout.lines[0].x, 0.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.minX, 0.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.maxX, width, kEpsilon);

    options.horizontalAlign = HorizontalAlign::Center;
    layout = Layout("Hello", options);
    EXPECT_NEAR(layout.lines[0].x, -width / 2.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.minX, -layout.bounds.maxX, kEpsilon);

    options.horizontalAlign = HorizontalAlign::Right;
    const TextLayout right = Layout("Hello", options);
    EXPECT_NEAR(right.bounds.maxX, 0.0f, kEpsilon);
    EXPECT_NEAR(right.bounds.minX, -width, kEpsilon);

    // Quads move with their line
    const TextLayout left = Layout("Hello");
    EXPECT_NEAR(right.quads[0].position.minX, left.quads[0].position.minX - width, kEpsilon);
}

TEST_P(TextLayoutTest, EachLineIsAlignedOnItsOwn)
{
    LayoutOptions options;
    options.horizontalAlign = HorizontalAlign::Center;
    const TextLayout layout = Layout("Hi\nHello there", options);
    ASSERT_EQ(layout.lines.size(), 2u);
    EXPECT_NEAR(layout.lines[0].x, -Width("Hi") / 2.0f, kEpsilon);
    EXPECT_NEAR(layout.lines[1].x, -Width("Hello there") / 2.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.Width(), Width("Hello there"), kEpsilon);
}

TEST_P(TextLayoutTest, VerticalAlignmentPlacesTheBlock)
{
    const FontMetrics &metrics = DefaultFont().GetMetrics();
    const float ascent = static_cast<float>(metrics.ascent) * Scale();
    const float descent = static_cast<float>(metrics.descent) * Scale();

    LayoutOptions options;
    options.verticalAlign = VerticalAlign::Top;
    TextLayout layout = Layout("one\ntwo", options);
    const float height = (ascent - descent) + layout.lineHeight;
    EXPECT_NEAR(layout.bounds.maxY, 0.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.minY, -height, kEpsilon);
    EXPECT_NEAR(layout.lines[0].baseline, -ascent, kEpsilon);

    options.verticalAlign = VerticalAlign::Middle;
    layout = Layout("one\ntwo", options);
    EXPECT_NEAR(layout.bounds.maxY, height / 2.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.minY, -height / 2.0f, kEpsilon);

    options.verticalAlign = VerticalAlign::Bottom;
    layout = Layout("one\ntwo", options);
    EXPECT_NEAR(layout.bounds.minY, 0.0f, kEpsilon);
    EXPECT_NEAR(layout.lines[1].baseline, -descent, kEpsilon);

    options.verticalAlign = VerticalAlign::Baseline;
    layout = Layout("one\ntwo", options);
    EXPECT_NEAR(layout.lines[0].baseline, 0.0f, kEpsilon);
    EXPECT_NEAR(layout.bounds.maxY, ascent, kEpsilon);
}

TEST_P(TextLayoutTest, BoundsCoverTheGlyphQuadsHorizontally)
{
    const TextLayout layout = Layout("Bounds\nare wide");
    ASSERT_FALSE(layout.quads.empty());
    // The quads include the SDF border, and their pixel edges are rounded outwards, so allow the spread
    // plus a pixel on each side
    const AtlasSettings &settings = DefaultFont().GetAtlas().GetSettings();
    const float slack = (static_cast<float>(settings.spreadPx) + 1.0f) / settings.basePx + kEpsilon;
    for (const GlyphQuad &quad : layout.quads)
    {
        EXPECT_GE(quad.position.minX, layout.bounds.minX - slack);
        EXPECT_LE(quad.position.maxX, layout.bounds.maxX + slack);
        EXPECT_GE(quad.position.minY, layout.bounds.minY - slack);
        EXPECT_LE(quad.position.maxY, layout.bounds.maxY + slack);
    }
}

TEST_P(TextLayoutTest, MissingGlyphsUseTheFallbackAndAreListed)
{
    // U+4E2D isn't in the font; U+00E9 is
    const TextLayout layout = Layout("a\xE4\xB8\xAD\xC3\xA9\xE4\xB8\xAD");
    ASSERT_EQ(layout.quads.size(), 4u);
    const AtlasGlyph *fallback = DefaultFont().GetAtlas().GetFallbackGlyph();
    ASSERT_NE(fallback, nullptr);

    EXPECT_EQ(layout.quads[1].codepoint, char32_t{0x4E2D});
    EXPECT_EQ(layout.quads[1].uv, fallback->uv);
    EXPECT_EQ(layout.quads[1].cluster, 1u);
    EXPECT_EQ(layout.quads[2].cluster, 4u);
    EXPECT_EQ(layout.quads[3].cluster, 6u);
    EXPECT_EQ(layout.missingCodepoints, std::vector<char32_t>{0x4E2D});
    EXPECT_NEAR(layout.lines[0].width, Advance(U'a') + 2.0f * fallback->advance + Advance(0xE9), kEpsilon);
}

TEST_P(TextLayoutTest, MissingGlyphsAreReportedOncePerFont)
{
    const auto font = MakeFont(GetParam());
    ASSERT_NE(font, nullptr);
    std::vector<char32_t> reported;
    font->SetMissingGlyphHandler([&reported](const SdfFont &, const char32_t c) { reported.push_back(c); });

    EXPECT_FALSE(font->HasReportedMissingGlyph());
    (void)LayoutText(*font, "plain ascii");
    EXPECT_TRUE(reported.empty());

    const TextLayout first = LayoutText(*font, "\xC3\xA9 and \xC3\xB1"); // Latin-1, but this atlas is ASCII
    EXPECT_EQ(first.missingCodepoints, (std::vector<char32_t>{0xE9, 0xF1}));
    const TextLayout second = LayoutText(*font, "\xC3\xB1 again");
    EXPECT_EQ(second.missingCodepoints, std::vector<char32_t>{0xF1});

    EXPECT_EQ(reported, std::vector<char32_t>{0xE9});
    EXPECT_TRUE(font->HasReportedMissingGlyph());
}

TEST_P(TextLayoutTest, InvalidUtf8LaysOutAsReplacementCharacters)
{
    const TextLayout layout = Layout("a\xFF" "b\xE2\x82");
    ASSERT_EQ(layout.quads.size(), 4u);
    EXPECT_EQ(layout.quads[1].codepoint, kReplacementCharacter);
    EXPECT_EQ(layout.quads[3].codepoint, kReplacementCharacter);
    EXPECT_EQ(layout.quads[3].cluster, 3u);
}

TEST_P(TextLayoutTest, MultiByteCharactersKeepTheirByteOffsets)
{
    const TextLayout layout = Layout("\xC3\xA9t\xC3\xA9");
    ASSERT_EQ(layout.quads.size(), 3u);
    EXPECT_EQ(layout.quads[0].cluster, 0u);
    EXPECT_EQ(layout.quads[1].cluster, 2u);
    EXPECT_EQ(layout.quads[2].cluster, 3u);
    EXPECT_EQ(layout.lines[0].byteEnd, 5u);
}

TEST_P(TextLayoutTest, ACustomShaperReplacesTheBasicOne)
{
    const EverythingIsXShaper shaper;
    LayoutOptions options;
    options.shaper = &shaper;
    const TextLayout layout = Layout("abc", options);
    ASSERT_EQ(layout.quads.size(), 3u);
    const AtlasGlyph *x = DefaultFont().GetAtlas().FindCodepoint(U'x');
    for (const GlyphQuad &quad : layout.quads)
    {
        EXPECT_EQ(quad.uv, x->uv);
    }
    EXPECT_EQ(layout.quads[0].codepoint, U'a');
    EXPECT_NEAR(layout.lines[0].width, 3.0f * x->advance, kEpsilon);
}

INSTANTIATE_TEST_SUITE_P(Backends, TextLayoutTest, ::testing::ValuesIn(Backends()), BackendName);
