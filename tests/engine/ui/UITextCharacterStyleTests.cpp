// Per-character colour and scale on UIText (SetCharacterColor, SetCharacterScale): the same index, lifetime and bounds
// rules as the character offsets (UITextOffsetTests.cpp), the quad maths, the vertex colours of the glyph mesh, and Lua.
// Drawn frames are in SoftwareTextGoldenTests.cpp (SoftwareUIGoldenTest.UITextCharacter*).

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <text/TextLayout.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/TextDrawing.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

using namespace N2Engine;
using N2Engine::UI::UIText;

namespace
{
    // A layout with one quad per listed byte offset, each 10 x 10 at x = 10 * i
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

TEST(UITextCharacterStyleTest, ScalesGrowAndShrinkTheirQuadsAboutTheirCentres)
{
    const std::string text = "abc";
    const Text::TextLayout layout = QuadsAt({0, 1, 2});
    const Text::TextLayout scaled = UIText::ApplyCharacterScales(layout, text, {2.0f, 1.0f, 0.0f});
    ASSERT_EQ(scaled.quads.size(), 3u);
    EXPECT_FLOAT_EQ(scaled.quads[0].position.minX, -5.0f);
    EXPECT_FLOAT_EQ(scaled.quads[0].position.maxX, 15.0f);
    EXPECT_FLOAT_EQ(scaled.quads[0].position.minY, -5.0f);
    EXPECT_FLOAT_EQ(scaled.quads[0].position.maxY, 15.0f);
    EXPECT_FLOAT_EQ(scaled.quads[1].position.minX, 10.0f) << "scale 1: unchanged";
    EXPECT_FLOAT_EQ(scaled.quads[2].position.minX, 25.0f) << "scale 0: a point at its centre";
    EXPECT_FLOAT_EQ(scaled.quads[2].position.maxX, 25.0f);
    EXPECT_FLOAT_EQ(layout.quads[0].position.minX, 0.0f) << "the layout itself is not changed";
}

TEST(UITextCharacterStyleTest, ScaleThenOffsetMovesTheScaledQuad)
{
    const std::string text = "ab";
    const Text::TextLayout layout = QuadsAt({0, 1});
    const Text::TextLayout both = UIText::ApplyCharacterOffsets(UIText::ApplyCharacterScales(layout, text, {2.0f}), text,
                                                                {{3.0f, 4.0f}});
    EXPECT_FLOAT_EQ(both.quads[0].position.minX, -2.0f);
    EXPECT_FLOAT_EQ(both.quads[0].position.maxY, 19.0f);
}

TEST(UITextCharacterStyleTest, QuadColoursCountCharactersNotBytes)
{
    // "é" is two bytes: the glyph after it starts at byte 3 but is character 2; the space has no quad
    const std::string text = "\xC3\xA9 x";
    const Text::TextLayout layout = QuadsAt({0, 3});
    const std::vector<Common::Color> colors = UIText::CharacterQuadColors(
        layout, text, {Common::Color{1.0f, 0.0f, 0.0f, 1.0f}, Common::Color{0.0f, 1.0f, 0.0f, 1.0f},
                       Common::Color{0.0f, 0.0f, 1.0f, 0.5f}});
    ASSERT_EQ(colors.size(), 2u) << "one per quad";
    EXPECT_FLOAT_EQ(colors[0].r, 1.0f);
    EXPECT_FLOAT_EQ(colors[1].b, 1.0f) << "x is character 2";
    EXPECT_FLOAT_EQ(colors[1].a, 0.5f);

    const std::vector<Common::Color> some = UIText::CharacterQuadColors(layout, text, {Common::Color{0.0f, 0.0f, 0.0f, 1.0f}});
    ASSERT_EQ(some.size(), 2u);
    EXPECT_FLOAT_EQ(some[1].r, 1.0f) << "a character with no colour is white";
    EXPECT_TRUE(UIText::CharacterQuadColors(layout, text, {}).empty()) << "no colours: no vertex colours";
}

TEST(UITextCharacterStyleTest, TheGlyphMeshCarriesOneVertexColourPerQuad)
{
    const Text::TextLayout layout = QuadsAt({0, 1});
    const std::vector<Common::Color> colors{Common::Color{1.0f, 0.0f, 0.0f, 1.0f}, Common::Color{0.0f, 0.0f, 1.0f, 0.25f}};
    const Renderer::Common::MeshData data = Rendering::TextDrawing::BuildMesh(layout, colors);
    ASSERT_EQ(data.vertices.size(), 8u);
    for (int v = 0; v < 4; ++v)
    {
        EXPECT_FLOAT_EQ(data.vertices[v].color[0], 1.0f) << v;
        EXPECT_FLOAT_EQ(data.vertices[v].color[2], 0.0f) << v;
        EXPECT_FLOAT_EQ(data.vertices[4 + v].color[2], 1.0f) << v;
        EXPECT_FLOAT_EQ(data.vertices[4 + v].color[3], 0.25f) << v;
    }
    // No colours, or a list of the wrong size: white, as before
    for (const auto &mesh : {Rendering::TextDrawing::BuildMesh(layout),
                             Rendering::TextDrawing::BuildMesh(layout, std::vector<Common::Color>{Common::Color::Red})})
    {
        for (const auto &vertex : mesh.vertices)
        {
            EXPECT_FLOAT_EQ(vertex.color[0], 1.0f);
            EXPECT_FLOAT_EQ(vertex.color[1], 1.0f);
            EXPECT_FLOAT_EQ(vertex.color[3], 1.0f);
        }
    }
}

TEST(UITextCharacterStyleTest, SetAndClearFollowTheOffsetRules)
{
    const auto element = UI::UISystem::CreateText("CharacterStyle_Text", "Mmm.");
    UIText &text = *element->GetComponent<UIText>();

    text.SetCharacterColor(5, Common::Color{1.0f, 1.0f, 1.0f, 1.0f});
    EXPECT_TRUE(text.GetCharacterColors().empty()) << "white past the end adds nothing";
    text.SetCharacterColor(2, Common::Color{1.0f, 0.0f, 0.0f, 0.5f});
    ASSERT_EQ(text.GetCharacterColors().size(), 3u);
    EXPECT_FLOAT_EQ(text.GetCharacterColors()[2].a, 0.5f);
    EXPECT_FLOAT_EQ(text.GetCharacterColors()[0].g, 1.0f) << "filled with white";
    text.ClearCharacterColors();
    EXPECT_TRUE(text.GetCharacterColors().empty());

    text.SetCharacterScale(5, 1.0f);
    EXPECT_TRUE(text.GetCharacterScales().empty()) << "1 past the end adds nothing";
    text.SetCharacterScale(1, 1.5f);
    ASSERT_EQ(text.GetCharacterScales().size(), 2u);
    EXPECT_FLOAT_EQ(text.GetCharacterScales()[1], 1.5f);
    EXPECT_FLOAT_EQ(text.GetCharacterScales()[0], 1.0f) << "filled with 1";
    text.ClearCharacterScales();
    EXPECT_TRUE(text.GetCharacterScales().empty());
}

TEST(UITextCharacterStyleTest, ValuesAreClampedAndKeptAcrossSetText)
{
    const auto element = UI::UISystem::CreateText("CharacterStyle_Clamp", "ab");
    UIText &text = *element->GetComponent<UIText>();
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    text.SetCharacterColor(0, Common::Color{2.0f, -1.0f, nan, 0.5f});
    EXPECT_FLOAT_EQ(text.GetCharacterColors()[0].r, 1.0f);
    EXPECT_FLOAT_EQ(text.GetCharacterColors()[0].g, 0.0f);
    EXPECT_FLOAT_EQ(text.GetCharacterColors()[0].b, 1.0f) << "non-finite counts as 1";
    text.SetCharacterScale(0, -2.0f);
    EXPECT_FLOAT_EQ(text.GetCharacterScales()[0], 0.0f) << "negative counts as 0";
    text.SetCharacterScale(1, inf);
    EXPECT_TRUE(text.GetCharacterScales().size() == 1u) << "non-finite counts as 1, which past the end adds nothing";

    // Like the offsets: not cleared by a text change (a script that changes the text sets them again)
    text.SetText("cd");
    EXPECT_EQ(text.GetCharacterColors().size(), 1u);
    EXPECT_EQ(text.GetCharacterScales().size(), 1u);
}

TEST(UITextCharacterStyleTest, AScriptCanSetAndClearThem)
{
    ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
    sol::state &lua = Scripting::LuaRuntime::Instance().GetState();
    const sol::protected_function_result result = lua.safe_script(R"(
        local go = UI.CreateText('LuaCharacterStyle', 'Hey')
        local text = go:GetComponent('UIText')
        text:SetCharacterColor(2, Color.new(1, 0, 0, 0.5))
        text:SetCharacterScale(3, 1.5)
        text:SetCharacterColor(0, Color.new(0, 0, 0, 1)) -- index below 1: ignored, as SetCharacterOffset
        text:SetCharacterScale(-4, 2)
        lua_character_style_go = go
    )", sol::script_pass_on_error);
    if (!result.valid())
    {
        const sol::error error = result;
        FAIL() << error.what();
    }
    const sol::object object = lua["lua_character_style_go"];
    ASSERT_TRUE(object.valid());
    const auto go = object.as<Scripting::GameObjectRef>().Pin();
    const UIText *text = go->GetComponent<UIText>();
    ASSERT_NE(text, nullptr);
    ASSERT_EQ(text->GetCharacterColors().size(), 2u) << "Lua's index 2 is character 1";
    EXPECT_FLOAT_EQ(text->GetCharacterColors()[1].r, 1.0f);
    EXPECT_FLOAT_EQ(text->GetCharacterColors()[1].g, 0.0f);
    EXPECT_FLOAT_EQ(text->GetCharacterColors()[1].a, 0.5f);
    EXPECT_FLOAT_EQ(text->GetCharacterColors()[0].g, 1.0f) << "the index-0 call was ignored";
    ASSERT_EQ(text->GetCharacterScales().size(), 3u);
    EXPECT_FLOAT_EQ(text->GetCharacterScales()[2], 1.5f);

    const sol::protected_function_result cleared = lua.safe_script(R"(
        local t = lua_character_style_go:GetComponent('UIText')
        t:ClearCharacterColors()
        t:ClearCharacterScales()
        lua_character_style_go = nil
    )", sol::script_pass_on_error);
    ASSERT_TRUE(cleared.valid());
    EXPECT_TRUE(text->GetCharacterColors().empty());
    EXPECT_TRUE(text->GetCharacterScales().empty());
}
