#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Logger.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"
#include "engine/ui/Button.hpp"
#include "engine/ui/Canvas.hpp"
#include "engine/ui/Image.hpp"
#include "engine/ui/RectTransform.hpp"
#include "engine/ui/UISystem.hpp"
#include "engine/ui/UIText.hpp"

using namespace N2Engine;
using namespace N2Engine::Scripting;

// The UI as scripts see it: UI.CreateCanvas/CreateElement/CreateText/CreateButton, and the RectTransform,
// Canvas, Image, UIText and Button methods
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

TEST_F(LuaUITest, ConfiguresAButton)
{
    Run(R"(
        lua_ui_button_go = UI.CreateButton("Play", "Start")
        lua_ui_button = lua_ui_button_go:GetComponent("Button")
        lua_ui_button:SetNormalColor(Color.new(0.9, 0.9, 0.9, 1))
        lua_ui_button:SetHighlightedColor(Color.new(0.8, 0.8, 0.8, 1))
        lua_ui_button:SetPressedColor(Color.new(0.5, 0.5, 0.5, 1))
        lua_ui_button:SetDisabledColor(Color.new(0.2, 0.2, 0.2, 0.5))
        lua_ui_button:SetColorMultiplier(1.25)
        lua_ui_button:SetFadeDuration(0)
        lua_ui_added_button_go = UI.CreateElement("Added")
        lua_ui_added_button = lua_ui_added_button_go:AddComponent("Button")
    )");

    EXPECT_EQ(Eval<int>("lua_ui_button_go:GetLayer()"), Layers::UI);
    EXPECT_TRUE(Eval<bool>("lua_ui_button_go:GetComponent('Button') == lua_ui_button"));
    EXPECT_EQ(Eval<std::string>("lua_ui_button_go:FindChild('Label'):GetComponent('UIText'):GetText()"), "Start");
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetNormalColor().r"), 0.9f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetHighlightedColor().g"), 0.8f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetPressedColor().b"), 0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetDisabledColor().a"), 0.5f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetColorMultiplier()"), 1.25f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetFadeDuration()"), 0.0f);
    EXPECT_TRUE(Eval<bool>("lua_ui_button:IsInteractable()"));
    EXPECT_EQ(Eval<std::string>("lua_ui_button:GetState()"), "Normal");
    EXPECT_TRUE(Eval<bool>("lua_ui_button:GetTargetGraphic() == lua_ui_button_go:GetComponent('Image')"));
    EXPECT_TRUE(Eval<bool>("lua_ui_added_button:GetTargetGraphic() == nil")) << "no graphic on its object";

    Run("lua_ui_button:SetInteractable(false)");
    EXPECT_FALSE(Eval<bool>("lua_ui_button:IsInteractable()"));
    EXPECT_EQ(Eval<std::string>("lua_ui_button:GetState()"), "Disabled");
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_button:GetCurrentTint().r"), 0.25f) << "0.2 times the multiplier 1.25";
    EXPECT_FALSE(Eval<bool>("lua_ui_button:Click()"));

    // The target graphic: a UIText, back to the default with nil, and anything else is an error
    Run("lua_ui_button:SetTargetGraphic(lua_ui_button_go:FindChild('Label'):GetComponent('UIText'))");
    EXPECT_TRUE(Eval<bool>("lua_ui_button:GetTargetGraphic() == lua_ui_button_go:FindChild('Label'):GetComponent('UIText')"));
    Run("lua_ui_button:SetTargetGraphic(nil)");
    EXPECT_TRUE(Eval<bool>("lua_ui_button:GetTargetGraphic() == lua_ui_button_go:GetComponent('Image')"));
    EXPECT_FALSE(Lua().safe_script("lua_ui_button:SetTargetGraphic(lua_ui_button)", sol::script_pass_on_error).valid());
    EXPECT_TRUE(Eval<bool>("lua_ui_button:GetTargetGraphic() == lua_ui_button_go:GetComponent('Image')"));

    Run("lua_ui_button = nil; lua_ui_button_go = nil; lua_ui_added_button = nil; lua_ui_added_button_go = nil");
}

TEST_F(LuaUITest, ButtonOnClickListeners)
{
    std::vector<std::string> logged;
    const size_t logId = Logger::logEvent += [&logged](const std::string_view message, Logger::LogLevel)
    {
        logged.emplace_back(message);
    };
    // Unsubscribed however the test ends, so a failed ASSERT can't leave the logger writing to `logged`
    struct Unsubscribe
    {
        size_t id;
        ~Unsubscribe() { Logger::logEvent -= id; }
    } unsubscribe{logId};

    Run(R"(
        lua_ui_click_go = UI.CreateButton("Clicker")
        lua_ui_click_button = lua_ui_click_go:GetComponent("Button")
        lua_ui_click_log = {}
        lua_ui_first = lua_ui_click_button:AddOnClick(function() table.insert(lua_ui_click_log, "first") end)
        lua_ui_failing = lua_ui_click_button:AddOnClick(function() error("listener failure from Lua") end)
        lua_ui_last = lua_ui_click_button:AddOnClick(function() table.insert(lua_ui_click_log, "last") end)
    )");
    EXPECT_EQ(Eval<int>("lua_ui_click_button:GetOnClickListenerCount()"), 3);

    // A click through the pointer callbacks, as the dispatcher sends them
    const sol::object goObject = Lua()["lua_ui_click_go"];
    ASSERT_TRUE(goObject.valid());
    const auto buttonObject = goObject.as<GameObjectRef>().Pin();
    auto *button = buttonObject->GetComponent<UI::Button>();
    ASSERT_NE(button, nullptr);
    button->OnMouseEnter();
    button->OnMouseDown();
    EXPECT_EQ(Eval<std::string>("lua_ui_click_button:GetState()"), "Pressed");
    button->OnMouseUpAsButton();
    button->OnMouseUp();
    EXPECT_EQ(Eval<std::string>("lua_ui_click_button:GetState()"), "Highlighted");

    EXPECT_EQ(Eval<int>("#lua_ui_click_log"), 2) << "the error didn't stop the listener after it";
    EXPECT_EQ(Eval<std::string>("lua_ui_click_log[1]"), "first");
    EXPECT_EQ(Eval<std::string>("lua_ui_click_log[2]"), "last");
    bool errorLogged = false;
    for (const std::string &message : logged)
    {
        errorLogged = errorLogged || message.find("listener failure from Lua") != std::string::npos;
    }
    EXPECT_TRUE(errorLogged) << "the Lua error is logged";

    // RemoveOnClick, also from inside a listener, and ClearOnClick
    Run(R"(
        lua_ui_click_log = {}
        lua_ui_click_button:RemoveOnClick(lua_ui_first)
        lua_ui_click_button:RemoveOnClick(lua_ui_failing)
        lua_ui_once = lua_ui_click_button:AddOnClick(function()
            table.insert(lua_ui_click_log, "once")
            lua_ui_click_button:RemoveOnClick(lua_ui_once)
        end)
        assert(lua_ui_click_button:Click())
        assert(lua_ui_click_button:Click())
    )");
    EXPECT_EQ(Eval<int>("#lua_ui_click_log"), 3);
    EXPECT_EQ(Eval<std::string>("lua_ui_click_log[1]"), "last");
    EXPECT_EQ(Eval<std::string>("lua_ui_click_log[2]"), "once");
    EXPECT_EQ(Eval<std::string>("lua_ui_click_log[3]"), "last");

    Run("lua_ui_click_button:ClearOnClick()");
    EXPECT_EQ(Eval<int>("lua_ui_click_button:GetOnClickListenerCount()"), 0);

    Run("lua_ui_click_go = nil; lua_ui_click_button = nil; lua_ui_click_log = nil");
}

TEST_F(LuaUITest, MakesAWorldSpaceCanvas)
{
    Run(R"(
        lua_ui_world_go = UI.CreateCanvas("World", "WorldSpace")
        lua_ui_world = lua_ui_world_go:GetComponent("Canvas")
        lua_ui_overlay_go = UI.CreateCanvas("Overlay")
        lua_ui_overlay = lua_ui_overlay_go:GetComponent("Canvas")
    )");
    EXPECT_EQ(Eval<std::string>("lua_ui_world:GetRenderMode()"), "WorldSpace");
    EXPECT_TRUE(Eval<bool>("lua_ui_world:IsWorldSpace()"));
    EXPECT_EQ(Eval<int>("lua_ui_world_go:GetLayer()"), Layers::UI);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_world_go:GetPositionable():GetScale().x"), 0.01f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_world:GetSize().x"), 100.0f);
    EXPECT_EQ(Eval<std::string>("lua_ui_overlay:GetRenderMode()"), "ScreenSpaceOverlay");
    EXPECT_FALSE(Eval<bool>("lua_ui_overlay:IsWorldSpace()"));

    Run(R"(lua_ui_world:SetSize(Vector2(400, 300)))");
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_world:GetSize().y"), 300.0f);
    EXPECT_FLOAT_EQ(Eval<float>("lua_ui_world_go:GetComponent('RectTransform'):GetSizeDelta().x"), 400.0f);

    // Switching an overlay canvas to world space gives it a transform and a RectTransform
    Run(R"(lua_ui_overlay:SetRenderMode("WorldSpace"))");
    EXPECT_EQ(Eval<std::string>("lua_ui_overlay:GetRenderMode()"), "WorldSpace");
    EXPECT_TRUE(Eval<bool>("lua_ui_overlay_go:GetPositionable() ~= nil"));
    EXPECT_TRUE(Eval<bool>("lua_ui_overlay_go:GetComponent('RectTransform') ~= nil"));
    Run(R"(lua_ui_overlay:SetRenderMode("ScreenSpaceOverlay"))");
    EXPECT_EQ(Eval<std::string>("lua_ui_overlay:GetRenderMode()"), "ScreenSpaceOverlay");

    // Unknown modes raise errors and change nothing
    EXPECT_FALSE(Lua().safe_script(R"(lua_ui_world:SetRenderMode("ScreenSpaceCamera"))", sol::script_pass_on_error)
                     .valid());
    EXPECT_EQ(Eval<std::string>("lua_ui_world:GetRenderMode()"), "WorldSpace");
    EXPECT_FALSE(Lua().safe_script(R"(UI.CreateCanvas("Bad", "Sideways"))", sol::script_pass_on_error).valid());

    Run("lua_ui_world_go = nil; lua_ui_world = nil; lua_ui_overlay_go = nil; lua_ui_overlay = nil");
}
