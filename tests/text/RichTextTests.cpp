#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

#include "text/RichText.hpp"
#include "text/Utf8.hpp"

// Issue #445: the rich text parser: each tag, nesting, and what bad markup gives (the text as written)

using namespace N2Engine::Text;

namespace
{
    RichText Parse(const std::string &markup) { return ParseRichText(markup, 20.0f); }

    /// The text is shown exactly as written, unstyled
    void ExpectLiteral(const std::string &markup)
    {
        const RichText parsed = Parse(markup);
        EXPECT_EQ(parsed.text, markup);
        EXPECT_EQ(parsed.styles.size(), DecodeUtf8(markup).size()) << markup;
        for (const RichStyle &style : parsed.styles)
        {
            EXPECT_EQ(style, RichStyle{}) << markup;
        }
    }
}

TEST(RichTextTest, PlainTextIsUnchanged)
{
    ExpectLiteral("");
    ExpectLiteral("Hello, world!\nSecond line");
    ExpectLiteral("a < b and c > d");
    ExpectLiteral("\xC3\xA9t\xC3\xA9 \xE2\x82\xAC");
}

TEST(RichTextTest, BoldAndItalicStyleWhatTheyEnclose)
{
    const RichText parsed = Parse("a<b>b<i>c</i></b><I>d</I>e");
    EXPECT_EQ(parsed.text, "abcde");
    ASSERT_EQ(parsed.styles.size(), 5u);
    EXPECT_FALSE(parsed.styles[0].bold);
    EXPECT_TRUE(parsed.styles[1].bold);
    EXPECT_FALSE(parsed.styles[1].italic);
    EXPECT_TRUE(parsed.styles[2].bold);
    EXPECT_TRUE(parsed.styles[2].italic);
    EXPECT_FALSE(parsed.styles[3].bold);
    EXPECT_TRUE(parsed.styles[3].italic) << "tag names in either case";
    EXPECT_EQ(parsed.styles[4], RichStyle{});
}

TEST(RichTextTest, ColoursInEveryForm)
{
    const RichText parsed = Parse("<color=#f00>a</color><color=#00FF0080>b</color><color=\"Blue\">c</color>"
                                  "<color=#102030>d</color>");
    EXPECT_EQ(parsed.text, "abcd");
    ASSERT_EQ(parsed.styles.size(), 4u);
    for (const RichStyle &style : parsed.styles)
    {
        EXPECT_TRUE(style.hasColor);
    }
    EXPECT_FLOAT_EQ(parsed.styles[0].colorR, 1.0f);
    EXPECT_FLOAT_EQ(parsed.styles[0].colorG, 0.0f);
    EXPECT_FLOAT_EQ(parsed.styles[0].colorA, 1.0f);
    EXPECT_FLOAT_EQ(parsed.styles[1].colorG, 1.0f);
    EXPECT_FLOAT_EQ(parsed.styles[1].colorA, 128.0f / 255.0f);
    EXPECT_FLOAT_EQ(parsed.styles[2].colorB, 1.0f);
    EXPECT_FLOAT_EQ(parsed.styles[3].colorR, 16.0f / 255.0f);
    EXPECT_FLOAT_EQ(parsed.styles[3].colorB, 48.0f / 255.0f);
}

TEST(RichTextTest, SizesAreRelativeToTheBaseAndNest)
{
    const RichText parsed = Parse("<size=40>a<size=50%>b</size>c</size>d<size=12.5>e</size><size=1000000>f");
    EXPECT_EQ(parsed.text, "abcdef");
    ASSERT_EQ(parsed.styles.size(), 6u);
    EXPECT_FLOAT_EQ(parsed.styles[0].scale, 2.0f);
    EXPECT_FLOAT_EQ(parsed.styles[1].scale, 0.5f) << "of the base size, not of the outer tag's";
    EXPECT_FLOAT_EQ(parsed.styles[2].scale, 2.0f);
    EXPECT_FLOAT_EQ(parsed.styles[3].scale, 1.0f);
    EXPECT_FLOAT_EQ(parsed.styles[4].scale, 0.625f);
    EXPECT_FLOAT_EQ(parsed.styles[5].scale, kMaxRichScale);
}

TEST(RichTextTest, OverlappingTagsWorkAsWritten)
{
    const RichText parsed = Parse("<b><i>a</b>b</i>c");
    EXPECT_EQ(parsed.text, "abc");
    EXPECT_TRUE(parsed.styles[0].bold && parsed.styles[0].italic);
    EXPECT_TRUE(!parsed.styles[1].bold && parsed.styles[1].italic);
    EXPECT_EQ(parsed.styles[2], RichStyle{});
}

TEST(RichTextTest, ATagLeftOpenAppliesToTheEnd)
{
    const RichText parsed = Parse("a<color=red>bc");
    EXPECT_EQ(parsed.text, "abc");
    EXPECT_FALSE(parsed.styles[0].hasColor);
    EXPECT_TRUE(parsed.styles[2].hasColor);
}

TEST(RichTextTest, BadMarkupIsShownAsWritten)
{
    for (const char *markup :
         {"<u>x</u>", "<b >x", "< b>x", "<b=1>x", "</b>", "a</i>b", "<color>x", "<color=>x", "<color=#12>x",
          "<color=#12345>x", "<color=#gg0000>x", "<color=octarine>x", "</color>x", "<size>x", "<size=>x", "<size=0>x",
          "<size=-4>x", "<size=abc>x", "<size=1.2.3>x", "<size=%>x", "<size=12px>x", "</size>", "<>", "</>", "<",
          "a<b", "<<<", "</noparse>", "<color=red </color>",
          "<aVeryLongTagNameThatIsNoTagAtAllAndGoesOnAndOn>x"})
    {
        ExpectLiteral(markup);
    }
}

TEST(RichTextTest, ATagAfterAStrayBracketStillWorks)
{
    const RichText parsed = Parse("<<b>x</b> 1<2<i>y");
    EXPECT_EQ(parsed.text, "<x 1<2y");
    EXPECT_TRUE(parsed.styles[1].bold);
    EXPECT_TRUE(parsed.styles.back().italic);
}

TEST(RichTextTest, NoParseShowsTagsAsText)
{
    const RichText parsed = Parse("<b><noparse><i>x</i></NoParse>y</b>");
    EXPECT_EQ(parsed.text, "<i>x</i>y");
    for (const RichStyle &style : parsed.styles)
    {
        EXPECT_TRUE(style.bold);
        EXPECT_FALSE(style.italic);
    }
    EXPECT_EQ(Parse("<noparse><b>unclosed").text, "<b>unclosed");
}

TEST(RichTextTest, ABadBaseSizeMakesAbsoluteSizesUnreadable)
{
    EXPECT_EQ(ParseRichText("<size=10>x", 0.0f).text, "<size=10>x");
    EXPECT_EQ(ParseRichText("<size=200%>x", 0.0f).text, "x");
}

TEST(RichTextTest, ThereIsOneStylePerCodePointEvenForBadUtf8)
{
    const std::string markup = "<b>\xE2\x82" "A\xFF</b>\xC3";
    const RichText parsed = Parse(markup);
    EXPECT_EQ(parsed.text, "\xE2\x82" "A\xFF\xC3");
    EXPECT_EQ(parsed.styles.size(), DecodeUtf8(parsed.text).size());
}
