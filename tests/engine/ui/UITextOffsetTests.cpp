#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <text/TextLayout.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

using namespace N2Engine;
using N2Engine::UI::UIText;

namespace
{
    // A layout with one quad per listed byte offset, each 10 wide at x = 10 * i
    Text::TextLayout QuadsAt(const std::vector<std::uint32_t> &clusters)
    {
        Text::TextLayout layout;
        float x = 0.0f;
        for (const std::uint32_t cluster : clusters)
        {
            Text::GlyphQuad q;
            q.position = Text::Rect{x, 0.0f, x + 10.0f, 10.0f};
            q.cluster = cluster;
            layout.quads.push_back(q);
            x += 10.0f;
        }
        return layout;
    }
}

TEST(UITextOffsetTest, OffsetsMoveTheirCharactersQuadsOnly)
{
    const std::string text = "Mmm.";
    const Text::TextLayout layout = QuadsAt({0, 1, 2, 3});
    const Text::TextLayout moved = UIText::ApplyCharacterOffsets(layout, text, {{0.0f, 5.0f}, {0.0f, 0.0f}, {2.0f, -3.0f}});
    ASSERT_EQ(moved.quads.size(), 4u);
    EXPECT_FLOAT_EQ(moved.quads[0].position.minY, 5.0f);
    EXPECT_FLOAT_EQ(moved.quads[0].position.maxY, 15.0f);
    EXPECT_FLOAT_EQ(moved.quads[1].position.minY, 0.0f);
    EXPECT_FLOAT_EQ(moved.quads[2].position.minX, 22.0f);
    EXPECT_FLOAT_EQ(moved.quads[2].position.maxY, 7.0f);
    EXPECT_FLOAT_EQ(moved.quads[3].position.minX, 30.0f) << "no offset given: unmoved";
    EXPECT_FLOAT_EQ(layout.quads[0].position.minY, 0.0f) << "the layout itself is not changed";
}

TEST(UITextOffsetTest, IndicesCountCharactersNotBytes)
{
    // "é" is two bytes: the glyph after it starts at byte 3 but is character 2; the space has no quad
    const std::string text = "\xC3\xA9 x";
    const Text::TextLayout layout = QuadsAt({0, 3});
    const Text::TextLayout moved = UIText::ApplyCharacterOffsets(layout, text, {{0.0f, 0.0f}, {9.0f, 9.0f}, {0.0f, 4.0f}});
    EXPECT_FLOAT_EQ(moved.quads[0].position.minY, 0.0f);
    EXPECT_FLOAT_EQ(moved.quads[1].position.minY, 4.0f) << "x is character 2";
}

TEST(UITextOffsetTest, SetAndClearOnTheComponent)
{
    const auto element = UI::UISystem::CreateText("Offsets_Text", "Mmm.");
    UIText &text = *element->GetComponent<UIText>();
    text.SetCharacterOffset(5, {0.0f, 0.0f});
    EXPECT_TRUE(text.GetCharacterOffsets().empty()) << "a zero offset past the end adds nothing";
    text.SetCharacterOffset(2, {1.0f, 2.0f});
    ASSERT_EQ(text.GetCharacterOffsets().size(), 3u);
    EXPECT_FLOAT_EQ(text.GetCharacterOffsets()[2].y, 2.0f);
    EXPECT_FLOAT_EQ(text.GetCharacterOffsets()[0].y, 0.0f);
    text.ClearCharacterOffsets();
    EXPECT_TRUE(text.GetCharacterOffsets().empty());
}
