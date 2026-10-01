#include <gtest/gtest.h>

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/Window.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/InputValue.hpp"
#include "engine/scripting/LuaHandles.hpp"
#include "engine/scripting/LuaRuntime.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;
using namespace N2Engine::Scripting;
using json = nlohmann::json;

namespace
{
    // A binding with a fixed value, so actions can be driven without a window or gamepad
    class FakeBinding final : public InputBinding
    {
    public:
        explicit FakeBinding(InputValue value) : InputBinding(static_cast<GLFWwindow *>(nullptr)), value(value) {}
        InputValue getValue() override { return value; }
        [[nodiscard]] BindingType GetType() const override { return BindingType::KeyboardButton; }
        [[nodiscard]] json Serialize() const override { return json::object(); }
        InputValue value;
    };
}

// Scripts keep action maps and actions (e.g. in self, or a subscription's closure). A map can be replaced or
// reloaded and an action replaced or removed; using a kept one afterwards must be a Lua error, not a use of
// freed memory. Headless: an uninitialized Window has no GLFW window, and the handles are pushed directly
// (Input.GetActionMap goes through the Application's window, which tests don't open).
class LuaInputHandleTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    }

    static sol::state &Lua() { return LuaRuntime::Instance().GetState(); }

    // Runs a chunk and fails the test with the Lua error message if it errors
    static void Run(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (!result.valid())
        {
            const sol::error err = result;
            FAIL() << "Lua error: " << err.what() << "\nin: " << code;
        }
    }

    // Runs a chunk that should fail and returns its error message
    static std::string RunExpectingError(const std::string &code)
    {
        const sol::protected_function_result result = Lua().safe_script(code, sol::script_pass_on_error);
        if (result.valid())
        {
            ADD_FAILURE() << "expected a Lua error from: " << code;
            return {};
        }
        const sol::error err = result;
        return err.what();
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

    static bool Contains(const std::string &text, const std::string &part)
    {
        return text.find(part) != std::string::npos;
    }

    static json EmptyMap() { return {{"actions", json::object()}}; }

    Window _window;
    InputSystem _input{_window};

    /// "Live", the active map: "Poke" (held down, so it starts on the first update) and "Idle"
    void SetUp() override
    {
        _input.MakeActionMap("Live", [](ActionMap *map)
        {
            map->MakeInputAction("Poke", [](InputAction *action)
            {
                action->AddBinding(std::make_unique<FakeBinding>(true));
            });
            map->MakeInputAction("Idle", [](InputAction *) {});
        });
        ASSERT_NE(_input.LoadActionMap("Live"), nullptr);
        Lua()["input_map"] = ActionMapRef(*_input.GetActionMap("Live"));
    }

    void TearDown() override
    {
        for (const char *name : {"input_map", "input_new_map", "input_action", "input_idle", "input_seen",
                                 "input_event", "input_hits", "input_weak", "input_replace_map",
                                 "input_valid_in_callback", "input_name_in_callback"})
        {
            Lua()[name] = sol::lua_nil;
        }
    }
};

TEST_F(LuaInputHandleTest, LiveHandlesKeepWorking)
{
    Run("input_action = input_map:Get('Poke')");

    EXPECT_TRUE(Eval<bool>("input_map:IsValid() and input_action:IsValid()"));
    EXPECT_EQ(Eval<std::string>("input_map.name"), "Live");
    EXPECT_EQ(Eval<std::string>("input_action:GetName()"), "Poke");
    EXPECT_EQ(Eval<std::string>("input_map['Idle']:GetName()"), "Idle");
    EXPECT_TRUE(Eval<bool>("input_map:Get('Missing') == nil"));
    EXPECT_TRUE(Eval<bool>("input_map['Missing'] == nil"));

    Run("input_map.disabled = true");
    EXPECT_TRUE(_input.GetActionMap("Live")->disabled);
    Run("input_map.disabled = false");
    EXPECT_FALSE(Eval<bool>("input_map.disabled"));

    // A subscription's callback gets a handle to the action too
    Run(R"(
        input_hits = 0
        input_action:Subscribe(function(action)
            input_hits = input_hits + 1
            input_seen = action
        end)
    )");
    _input.Update();

    EXPECT_EQ(Eval<int>("input_hits"), 1);
    EXPECT_TRUE(Eval<bool>("input_seen:WasStarted()"));
    EXPECT_TRUE(Eval<bool>("input_seen == input_action"));
}

TEST_F(LuaInputHandleTest, HandlesToTheSameObjectAreEqual)
{
    EXPECT_TRUE(Eval<bool>("input_map:Get('Poke') == input_map:Get('Poke')"));
    EXPECT_FALSE(Eval<bool>("input_map:Get('Poke') == input_map:Get('Idle')"));
    EXPECT_FALSE(Eval<bool>("input_map:Get('Poke') == input_map")) << "different kinds of object";

    Lua()["input_new_map"] = ActionMapRef(*_input.GetActionMap("Live"));
    EXPECT_TRUE(Eval<bool>("input_new_map == input_map"));
}

TEST_F(LuaInputHandleTest, HandlesErrorAfterTheMapIsReplaced)
{
    Run("input_action = input_map:Get('Poke')");

    // Frees the old map and its actions (what Input.CreateActionMap with an existing name does)
    ASSERT_NE(_input.CreateActionMapFromJson("Live", EmptyMap()), nullptr);

    EXPECT_FALSE(Eval<bool>("input_map:IsValid()"));
    EXPECT_FALSE(Eval<bool>("input_action:IsValid()"));
    // These used to reach the freed map and action
    std::string error = RunExpectingError("input_map:Get('Poke')");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed ActionMap")) << error;
    error = RunExpectingError("return input_map.name");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed ActionMap")) << error;
    error = RunExpectingError("input_action:GetPhase()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed InputAction")) << error;
    error = RunExpectingError("input_action:Subscribe(function() end)");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed InputAction")) << error;

    // A handle to the replacement works, and is a different map
    Lua()["input_new_map"] = ActionMapRef(*_input.GetActionMap("Live"));
    EXPECT_TRUE(Eval<bool>("input_new_map:IsValid() and input_new_map:Get('Poke') == nil"));
    EXPECT_FALSE(Eval<bool>("input_new_map == input_map"));
}

TEST_F(LuaInputHandleTest, ActionHandlesErrorAfterRemoveOrReplace)
{
    Run("input_action = input_map:Get('Poke') input_idle = input_map:Get('Idle')");
    ActionMap *map = _input.GetActionMap("Live");

    ASSERT_TRUE(map->RemoveInputAction("Poke"));
    EXPECT_FALSE(Eval<bool>("input_action:IsValid()"));
    const std::string error = RunExpectingError("input_action:SetDisabled(true)");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed InputAction")) << error;

    // Adding one with the same name frees the old one
    map->AddInputAction(std::make_unique<InputAction>("Idle"));
    EXPECT_FALSE(Eval<bool>("input_idle:IsValid()"));

    // The map itself is unaffected
    EXPECT_TRUE(Eval<bool>("input_map:IsValid() and input_map:Get('Poke') == nil"));
    EXPECT_TRUE(Eval<bool>("input_map:Get('Idle'):IsValid()"));
    EXPECT_FALSE(Eval<bool>("input_map:Get('Idle') == input_idle"));
}

TEST_F(LuaInputHandleTest, ReplacedFromItsOwnCallbackErrorsOnceTheUpdateEnds)
{
    // Stands in for Input.CreateActionMap, which needs the Application's window
    Lua().set_function("input_replace_map", [this] { _input.CreateActionMapFromJson("Live", EmptyMap()); });
    Run(R"(
        input_action = input_map:Get('Poke')
        input_action:Subscribe(function(action)
            input_replace_map()
            -- Replaced while its update is running, so only freed when the update ends
            input_valid_in_callback = action:IsValid() and input_map:IsValid()
            input_name_in_callback = action:GetName()
        end)
    )");

    _input.Update();

    EXPECT_TRUE(Eval<bool>("input_valid_in_callback"));
    EXPECT_TRUE(Eval<bool>("input_name_in_callback == 'Poke'"));

    // The update is over: the old map and its actions are freed
    EXPECT_FALSE(Eval<bool>("input_map:IsValid()"));
    EXPECT_FALSE(Eval<bool>("input_action:IsValid()"));
    std::string error = RunExpectingError("input_action:GetBoolValue()");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed InputAction")) << error;
    error = RunExpectingError("input_map:Get('Poke')");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed ActionMap")) << error;

    _input.Update(); // the replacement runs normally
}

TEST_F(LuaInputHandleTest, OnStateChangedIsTheActionItself)
{
    Run(R"(
        input_hits = 0
        input_event = input_map:Get('Poke'):OnStateChanged()
        input_event:Subscribe(function() input_hits = input_hits + 1 end)
    )");
    _input.Update();

    EXPECT_EQ(Eval<int>("input_hits"), 1);
    EXPECT_TRUE(Eval<bool>("input_event == input_map:Get('Poke')"));

    // It used to be a reference to the event inside the action, which dangled once the action was freed
    ASSERT_NE(_input.CreateActionMapFromJson("Live", EmptyMap()), nullptr);
    const std::string error = RunExpectingError("input_event:Subscribe(function() end)");
    EXPECT_TRUE(Contains(error, "attempt to use a destroyed InputAction")) << error;
}

TEST_F(LuaInputHandleTest, SubscriptionIsReleasedWithItsAction)
{
    // The event, so the callback, lives on the action: nothing else holds the callback once it's freed
    Run(R"(
        input_weak = setmetatable({}, { __mode = "v" })
        local callback = function() end
        input_weak[1] = callback
        input_map:Get('Poke'):Subscribe(callback)
    )");
    Run("collectgarbage() collectgarbage()");
    ASSERT_TRUE(Eval<bool>("input_weak[1] ~= nil")) << "the live action's event holds its callback";

    ASSERT_NE(_input.CreateActionMapFromJson("Live", EmptyMap()), nullptr);
    Run("collectgarbage() collectgarbage()");

    EXPECT_TRUE(Eval<bool>("input_weak[1] == nil"));
}
