#include <gtest/gtest.h>

#include <string>

#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

// The UI as scripts see it: UI.CreateCanvas/CreateElement/CreateText, and the RectTransform, Canvas, Image and
// UIText methods
class LuaUITest : public ::testing::Test
{
protected:
    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    template <typename T>
    static T Eval(const std::string &expression)
    {
        const sol::protected_function_result result =
            Lua().safe_script("return " + expression, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            ADD_FAILURE() << "Lua error: " << err.what() << "\nin: " << expression;
            return T{};
        }
        return result.get<T>();
    }
};

TEST_F(LuaUITest, BuildsAndLaysOutAnElement)
{
    const auto scene = Scene::Create("LuaUI_Scene");

    Run(R"(
        lua_ui_canvas = UI.CreateCanvas("HUD")
        lua_ui_canvas:GetComponent("Canvas"):SetSortOrder(3)
        lua_ui_panel = UI.CreateElement("Panel")
        lua_ui_canvas:AddChild(lua_ui_panel)
        local rt = lua_ui_panel:GetComponent("RectTransform")
        rt:SetAnchorMin(Vector2(1, 1))
        rt:SetAnchorMax(Vector2(1, 1))
        rt:SetPivot(Vector2(1, 1))
        rt:SetAnchoredPosition(Vector2(-10, -10))
        rt:SetSizeDelta(Vector2(200, 50))
        local image = lua_ui_panel:AddComponent("Image")
        image:SetColor(Color.new(1, 0, 0, 1))
        image:SetRaycastTarget(false)
    )");

    EXPECT_EQ(Eval<int>("lua_ui_canvas:GetLayer()"), Layers::UI);
    EXPECT_EQ(Eval<int>("lua_ui_panel:GetLayer()"), Layers::UI);
    EXPECT_EQ(Eval<int>("lua_ui_canvas:GetComponent('Canvas'):GetSortOrder()"), 3);
    EXPECT_FALSE(Eval<bool>("lua_ui_panel:GetComponent('Image'):GetRaycastTarget()"));
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_panel:GetComponent('Image'):GetColor().g"), 0.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_panel:GetComponent('RectTransform'):GetOffsetMin().x"), -210.0f);

    // Lay it out from C++ and read the result back in Lua
    const sol::object canvasObject = Lua()["lua_ui_canvas"];
    ASSERT_TRUE(canvasObject.valid());
    const auto canvas = canvasObject.as<GameObjectRef>().Pin();
    scene->AddRootGameObject(canvas);
    static_cast<void>(UI::UISystem::CollectGraphics(*scene, Vector2i{800, 600}));

    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_panel:GetComponent('RectTransform'):GetRect().x"), 590.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_panel:GetComponent('RectTransform'):GetRect().y"), 540.0f);
    EXPECT_TRUE(Eval<bool>("lua_ui_panel:GetComponent('RectTransform'):GetRect():Contains(Vector2(600, 560))"));

    Run("lua_ui_canvas = nil; lua_ui_panel = nil");
}

TEST_F(LuaUITest, CreatesAndConfiguresText)
{
    const auto scene = Scene::Create("LuaUI_Text");

    Run(R"(
        lua_ui_text_canvas = UI.CreateCanvas("TextCanvas")
        lua_ui_label = UI.CreateText("Label", "Hello")
        lua_ui_text_canvas:AddChild(lua_ui_label)
        local rt = lua_ui_label:GetComponent("RectTransform")
        rt:SetAnchorMin(Vector2(0, 0))
        rt:SetAnchorMax(Vector2(0, 0))
        rt:SetPivot(Vector2(0, 0))
        rt:SetAnchoredPosition(Vector2(100, 50))
        rt:SetSizeDelta(Vector2(300, 100))
        lua_ui_text = lua_ui_label:GetComponent("UIText")
        lua_ui_text:SetFontSize(20)
        lua_ui_text:SetAlignment("Right", "Bottom")
        lua_ui_text:SetWrap(false)
        lua_ui_text:SetLineSpacing(1.5)
        lua_ui_text:SetLetterSpacing(0.1)
        lua_ui_text:SetColor(Color.new(0, 1, 0, 1))
        lua_ui_text:SetFont(nil)
        lua_ui_h, lua_ui_v = lua_ui_text:GetAlignment()
        lua_ui_added_go = UI.CreateElement("Added")
        lua_ui_added = lua_ui_added_go:AddComponent("UIText")
    )");

    EXPECT_EQ(Eval<int>("lua_ui_label:GetLayer()"), Layers::UI);
    EXPECT_EQ(Eval<std::string>("lua_ui_text:GetText()"), "Hello");
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_text:GetFontSize()"), 20.0f);
    EXPECT_FALSE(Eval<bool>("lua_ui_text:GetWrap()"));
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_text:GetLineSpacing()"), 1.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_text:GetLetterSpacing()"), 0.1f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_text:GetColor().g"), 1.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_text:GetColor().r"), 0.0f);
    EXPECT_FALSE(Eval<bool>("lua_ui_text:GetRaycastTarget()")) << "text lets the pointer through by default";
    EXPECT_EQ(Eval<std::string>("lua_ui_h"), "Right");
    EXPECT_EQ(Eval<std::string>("lua_ui_v"), "Bottom");
    EXPECT_TRUE(Eval<bool>("lua_ui_label:GetComponent('UIText') == lua_ui_text"));
    EXPECT_TRUE(Eval<bool>("lua_ui_added ~= nil and lua_ui_added:GetText() == ''"));

    Run("lua_ui_text:SetRaycastTarget(true)");
    EXPECT_TRUE(Eval<bool>("lua_ui_text:GetRaycastTarget()"));

    // An unknown alignment is an error and changes nothing; so is a font that doesn't load
    EXPECT_FALSE(Lua().safe_script(R"(lua_ui_text:SetAlignment("Left", "Sideways"))", sol::script_pass_on_error)
                     .valid());
    Run("lua_ui_h, lua_ui_v = lua_ui_text:GetAlignment()");
    EXPECT_EQ(Eval<std::string>("lua_ui_h"), "Right");
    EXPECT_EQ(Eval<std::string>("lua_ui_v"), "Bottom");
    EXPECT_FALSE(Lua().safe_script(R"(lua_ui_text:SetFont("res://fonts/NoSuchFont.ttf"))", sol::script_pass_on_error)
                     .valid());

    // Lay it out from C++; the bounds are in canvas space, right- and bottom-aligned in the rect
    const sol::object canvasObject = Lua()["lua_ui_text_canvas"];
    ASSERT_TRUE(canvasObject.valid());
    scene->AddRootGameObject(canvasObject.as<GameObjectRef>().Pin());
    static_cast<void>(UI::UISystem::CollectGraphics(*scene, Vector2i{800, 600}));

    Run("lua_ui_minX, lua_ui_minY, lua_ui_maxX, lua_ui_maxY = lua_ui_text:GetBounds()");
    EXPECT_NEAR(Eval<float>("lua_ui_maxX"), 400.0f, 1e-3f);
    EXPECT_NEAR(Eval<float>("lua_ui_minY"), 50.0f, 1e-3f);
    EXPECT_GT(Eval<float>("lua_ui_minX"), 100.0f);
    EXPECT_LT(Eval<float>("lua_ui_maxY"), 150.0f);

    Run("lua_ui_text_canvas = nil; lua_ui_label = nil; lua_ui_text = nil; lua_ui_added = nil; lua_ui_added_go = nil");
}
