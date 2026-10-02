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

using namespace N2Engine;
using namespace N2Engine::Scripting;

// The UI as scripts see it: UI.CreateCanvas/CreateElement, and the RectTransform, Canvas and Image methods
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
