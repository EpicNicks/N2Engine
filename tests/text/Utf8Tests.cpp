#include <gtest/gtest.h>

#include <string>

#include <text/Utf8.hpp>

using namespace N2Engine::Text;

namespace
{
    constexpr char32_t R = kReplacementCharacter;
}

TEST(Utf8DecodeTest, DecodesValidSequencesOfEveryLength)
{
    // 'A', U+00E9, U+20AC, U+1F600
    EXPECT_EQ(DecodeUtf8("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"), (std::u32string{U'A', 0xE9, 0x20AC, 0x1F600}));
    EXPECT_EQ(DecodeUtf8(""), std::u32string{});
    EXPECT_EQ(DecodeUtf8(std::string("a\0b", 3)), (std::u32string{U'a', 0, U'b'}));
}

TEST(Utf8DecodeTest, DecodesTheBoundaryCodepoints)
{
    EXPECT_EQ(DecodeUtf8("\x7F"), std::u32string{0x7F});
    EXPECT_EQ(DecodeUtf8("\xC2\x80"), std::u32string{0x80});
    EXPECT_EQ(DecodeUtf8("\xDF\xBF"), std::u32string{0x7FF});
    EXPECT_EQ(DecodeUtf8("\xE0\xA0\x80"), std::u32string{0x800});
    EXPECT_EQ(DecodeUtf8("\xED\x9F\xBF"), std::u32string{0xD7FF});
    EXPECT_EQ(DecodeUtf8("\xEE\x80\x80"), std::u32string{0xE000});
    EXPECT_EQ(DecodeUtf8("\xEF\xBF\xBF"), std::u32string{0xFFFF});
    EXPECT_EQ(DecodeUtf8("\xF0\x90\x80\x80"), std::u32string{0x10000});
    EXPECT_EQ(DecodeUtf8("\xF4\x8F\xBF\xBF"), std::u32string{0x10FFFF});
}

TEST(Utf8DecodeTest, OverlongFormsBecomeReplacementCharacters)
{
    // C0/C1 can only start overlong forms; E0 and F0 need a large enough second byte
    EXPECT_EQ(DecodeUtf8("\xC0\xAF"), (std::u32string{R, R}));
    EXPECT_EQ(DecodeUtf8("\xC1\xBF"), (std::u32string{R, R}));
    EXPECT_EQ(DecodeUtf8("\xE0\x80\xAF"), (std::u32string{R, R, R}));
    EXPECT_EQ(DecodeUtf8("\xF0\x80\x80\xAF"), (std::u32string{R, R, R, R}));
}

TEST(Utf8DecodeTest, SurrogatesBecomeReplacementCharacters)
{
    // U+D800 and U+DFFF encoded directly, and a CESU-8 style pair
    EXPECT_EQ(DecodeUtf8("\xED\xA0\x80"), (std::u32string{R, R, R}));
    EXPECT_EQ(DecodeUtf8("\xED\xBF\xBF"), (std::u32string{R, R, R}));
    EXPECT_EQ(DecodeUtf8("\xED\xA0\xBD\xED\xB8\x80"), (std::u32string{R, R, R, R, R, R}));
}

TEST(Utf8DecodeTest, ValuesAboveTheUnicodeRangeBecomeReplacementCharacters)
{
    EXPECT_EQ(DecodeUtf8("\xF4\x90\x80\x80"), (std::u32string{R, R, R, R}));
    EXPECT_EQ(DecodeUtf8("\xF5\x80\x80\x80"), (std::u32string{R, R, R, R}));
    EXPECT_EQ(DecodeUtf8("\xFF"), std::u32string{R});
}

TEST(Utf8DecodeTest, TruncatedSequencesBecomeOneReplacementEach)
{
    // The valid prefix of a cut-off sequence is one U+FFFD, and the byte that interrupted it decodes
    // on its own
    EXPECT_EQ(DecodeUtf8("\xE2\x82"), std::u32string{R});
    EXPECT_EQ(DecodeUtf8("\xE2\x82" "A"), (std::u32string{R, U'A'}));
    EXPECT_EQ(DecodeUtf8("\xF0\x9F\x98"), std::u32string{R});
    EXPECT_EQ(DecodeUtf8("\xC3"), std::u32string{R});
    EXPECT_EQ(DecodeUtf8("a\xC3" "b"), (std::u32string{U'a', R, U'b'}));
    EXPECT_EQ(DecodeUtf8("\xE2\x82\xE2\x82\xAC"), (std::u32string{R, 0x20AC}));
}

TEST(Utf8DecodeTest, StrayContinuationBytesBecomeReplacementCharacters)
{
    EXPECT_EQ(DecodeUtf8("\x80"), std::u32string{R});
    EXPECT_EQ(DecodeUtf8("a\xBF\xBF" "b"), (std::u32string{U'a', R, R, U'b'}));
}

TEST(Utf8DecodeTest, DecodeNextAdvancesByTheBytesItConsumed)
{
    const std::string text = "\xC3\xA9\xE2\x82" "x";
    std::size_t position = 0;
    EXPECT_EQ(DecodeUtf8Next(text, position), char32_t{0xE9});
    EXPECT_EQ(position, 2u);
    EXPECT_EQ(DecodeUtf8Next(text, position), R);
    EXPECT_EQ(position, 4u);
    EXPECT_EQ(DecodeUtf8Next(text, position), U'x');
    EXPECT_EQ(position, 5u);
    // At the end: U+FFFD, and the position stays put
    EXPECT_EQ(DecodeUtf8Next(text, position), R);
    EXPECT_EQ(position, 5u);
}

TEST(Utf8DecodeTest, EncodeRoundTripsAndReplacesInvalidValues)
{
    const std::u32string text{U'A', 0xE9, 0x20AC, 0x1F600, 0x10FFFF};
    EXPECT_EQ(DecodeUtf8(EncodeUtf8(text)), text);
    EXPECT_EQ(EncodeUtf8(std::u32string{0xD800, 0x110000}), "\xEF\xBF\xBD\xEF\xBF\xBD");
}
