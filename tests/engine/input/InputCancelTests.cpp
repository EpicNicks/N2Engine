#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <nlohmann/json.hpp>

#include "engine/Window.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/InputTypes.hpp"
#include "engine/input/InputValue.hpp"

using namespace N2Engine;
using namespace N2Engine::Input;
using json = nlohmann::json;

namespace
{
    // A binding the test presses and releases, so actions can be driven without a window or gamepad
    class HeldBinding final : public InputBinding
    {
    public:
        explicit HeldBinding(bool *held) : InputBinding(static_cast<GLFWwindow *>(nullptr)), held(held) {}
        InputValue getValue() override { return *held; }
        [[nodiscard]] BindingType GetType() const override { return BindingType::KeyboardButton; }
        [[nodiscard]] json Serialize() const override { return json::object(); }
        bool *held;
    };

    // A map with one action "Fire" bound to *held, counting its OnStateChanged calls
    std::unique_ptr<ActionMap> MapWithHeldAction(const std::string &name, bool *held, int *callbacks)
    {
        auto map = std::make_unique<ActionMap>(name);
        map->MakeInputAction("Fire", [held](InputAction *action)
        {
            action->AddBinding(std::make_unique<HeldBinding>(held));
        });
        (*map)["Fire"].GetOnStateChanged() += [callbacks](InputAction &) { ++*callbacks; };
        return map;
    }
}

// ============================================================================
// Disabling a map cancels its active actions
// ============================================================================

TEST(ActionMapCancelTest, DisablingAMapCancelsAHeldAction)
{
    bool held = true;
    int callbacks = 0;
    const auto map = MapWithHeldAction("Gameplay", &held, &callbacks);
    InputAction &fire = (*map)["Fire"];

    map->Update(); // Started
    map->Update(); // Performed
    ASSERT_EQ(fire.GetPhase(), ActionPhase::Performed);
    const int before = callbacks;

    map->disabled = true;
    map->Update();

    EXPECT_EQ(fire.GetPhase(), ActionPhase::Cancelled);
    EXPECT_FALSE(fire.IsActive());
    EXPECT_FALSE(fire.GetBoolValue()) << "the value is reset with the cancel";
    EXPECT_EQ(callbacks, before + 1) << "subscribers see the cancel once";

    map->Update();
    map->Update();
    EXPECT_EQ(callbacks, before + 1) << "no further callbacks while the map stays disabled";
}

TEST(ActionMapCancelTest, ReenabledMapStartsAStillHeldActionAgain)
{
    bool held = true;
    int callbacks = 0;
    const auto map = MapWithHeldAction("Gameplay", &held, &callbacks);
    InputAction &fire = (*map)["Fire"];
    map->Update();
    map->Update();
    map->disabled = true;
    map->Update();
    const int before = callbacks;

    map->disabled = false;
    map->Update();

    EXPECT_EQ(fire.GetPhase(), ActionPhase::Started);
    EXPECT_TRUE(fire.WasStarted()) << "Cancelled is reset to Waiting first, as re-enabling an action does";
    EXPECT_EQ(callbacks, before + 1);
}

TEST(ActionMapCancelTest, ReenabledMapWithInputReleasedStaysQuiet)
{
    bool held = true;
    int callbacks = 0;
    const auto map = MapWithHeldAction("Gameplay", &held, &callbacks);
    InputAction &fire = (*map)["Fire"];
    map->Update();
    map->disabled = true;
    map->Update();
    const int before = callbacks;

    held = false;
    map->disabled = false;
    map->Update();

    EXPECT_EQ(fire.GetPhase(), ActionPhase::Waiting);
    EXPECT_EQ(callbacks, before) << "the reset from Cancelled to Waiting is silent";
}

TEST(ActionMapCancelTest, DisablingAMapWithNothingActiveFiresNothing)
{
    bool held = false;
    int callbacks = 0;
    const auto map = MapWithHeldAction("Gameplay", &held, &callbacks);
    map->Update();
    ASSERT_EQ(callbacks, 0);

    map->disabled = true;
    map->Update();

    EXPECT_EQ(callbacks, 0);
    EXPECT_EQ((*map)["Fire"].GetPhase(), ActionPhase::Waiting);
}

TEST(ActionMapCancelTest, CancelActiveActionsIsImmediate)
{
    bool held = true;
    int callbacks = 0;
    const auto map = MapWithHeldAction("Gameplay", &held, &callbacks);
    map->Update();
    const int before = callbacks;

    map->CancelActiveActions();
    EXPECT_EQ((*map)["Fire"].GetPhase(), ActionPhase::Cancelled);
    EXPECT_EQ(callbacks, before + 1);

    map->CancelActiveActions();
    EXPECT_EQ(callbacks, before + 1) << "an action that isn't active is left alone";
}

// ============================================================================
// Switching maps cancels the active actions of the map left
// ============================================================================

class InputMapSwitchCancelTest : public ::testing::Test
{
protected:
    Window _window; // never opened: the input system is headless
    InputSystem _input{_window};
};

TEST_F(InputMapSwitchCancelTest, LoadingAnotherMapCancelsTheOldMapsActions)
{
    bool gameplayHeld = true;
    bool menuHeld = false;
    int gameplayCallbacks = 0;
    int menuCallbacks = 0;
    _input.AddActionMap(MapWithHeldAction("Gameplay", &gameplayHeld, &gameplayCallbacks));
    _input.AddActionMap(MapWithHeldAction("Menu", &menuHeld, &menuCallbacks));
    ASSERT_NE(_input.LoadActionMap("Gameplay"), nullptr);

    _input.Update();
    _input.Update();
    InputAction &fire = (*_input.GetActionMap("Gameplay"))["Fire"];
    ASSERT_EQ(fire.GetPhase(), ActionPhase::Performed);
    const int before = gameplayCallbacks;

    ASSERT_NE(_input.LoadActionMap("Menu"), nullptr);

    EXPECT_EQ(fire.GetPhase(), ActionPhase::Cancelled) << "cancelled when the map is left, not later";
    EXPECT_FALSE(fire.IsActive());
    EXPECT_EQ(gameplayCallbacks, before + 1);

    _input.Update();
    EXPECT_EQ(gameplayCallbacks, before + 1) << "the map left is no longer updated";
    EXPECT_EQ(menuCallbacks, 0);

    // Back again with the key still held: a fresh press
    ASSERT_NE(_input.LoadActionMap("Gameplay"), nullptr);
    _input.Update();
    EXPECT_TRUE(fire.WasStarted());
    EXPECT_EQ(gameplayCallbacks, before + 2);
}

TEST_F(InputMapSwitchCancelTest, ReloadingTheCurrentMapCancelsNothing)
{
    bool held = true;
    int callbacks = 0;
    _input.AddActionMap(MapWithHeldAction("Gameplay", &held, &callbacks));
    _input.LoadActionMap("Gameplay");
    _input.Update();
    _input.Update();
    const int before = callbacks;

    ASSERT_NE(_input.LoadActionMap("Gameplay"), nullptr);

    EXPECT_EQ((*_input.GetActionMap("Gameplay"))["Fire"].GetPhase(), ActionPhase::Performed);
    EXPECT_EQ(callbacks, before);
}

TEST_F(InputMapSwitchCancelTest, CancelCallbackCanReplaceTheMapItLeaves)
{
    bool held = true;
    int callbacks = 0;
    _input.AddActionMap(MapWithHeldAction("Gameplay", &held, &callbacks));
    _input.AddActionMap(std::make_unique<ActionMap>("Menu"));
    _input.LoadActionMap("Gameplay");
    _input.Update();

    bool replaced = false;
    (*_input.GetActionMap("Gameplay"))["Fire"].GetOnStateChanged() += [&](InputAction &action)
    {
        if (action.GetPhase() == ActionPhase::Cancelled && !replaced)
        {
            // Replaces the map whose cancel pass is running: it must be retired, not freed under it
            _input.AddActionMap(std::make_unique<ActionMap>("Gameplay"));
            replaced = true;
        }
    };

    EXPECT_NO_FATAL_FAILURE(_input.LoadActionMap("Menu"));
    EXPECT_TRUE(replaced);
    EXPECT_EQ(_input.GetActionMap("Gameplay")->Serialize()["actions"].size(), 0u);
}

TEST_F(InputMapSwitchCancelTest, SwitchingFromACallbackCancelsWhenTheLoopEnds)
{
    // Every action is held, and whichever one the loop reaches first switches to the menu, so the test does
    // not depend on the map's (unordered) iteration order
    bool held = true;
    const std::vector<std::string> names = {"Pause", "Fire", "Jump", "Crouch", "Interact", "Reload"};
    _input.MakeActionMap("Gameplay", [&](ActionMap *map)
    {
        for (const auto &name : names)
        {
            map->MakeInputAction(name, [&held](InputAction *action) { action->AddBinding(std::make_unique<HeldBinding>(&held)); });
        }
    });
    _input.AddActionMap(std::make_unique<ActionMap>("Menu"));
    _input.LoadActionMap("Gameplay");
    ActionMap *gameplay = _input.GetActionMap("Gameplay");

    int started = 0;
    int cancelled = 0;
    int afterSwitch = 0;
    bool switched = false;
    for (const auto &name : names)
    {
        (*gameplay)[name].GetOnStateChanged() += [&](InputAction &action)
        {
            if (action.GetPhase() == ActionPhase::Started)
            {
                ++started;
                if (switched)
                {
                    ++afterSwitch;
                }
                switched = true;
                _input.LoadActionMap("Menu"); // e.g. a pause button opening a menu
            }
            else if (action.GetPhase() == ActionPhase::Cancelled)
            {
                ++cancelled;
            }
        };
    }

    _input.Update();

    ASSERT_NE(_input.GetCurActionMap(), nullptr);
    EXPECT_EQ(_input.GetCurActionMap()->name, "Menu");
    EXPECT_EQ(started, 1) << "the loop went on polling the map it had left";
    EXPECT_EQ(afterSwitch, 0) << "an action started after the switch";
    EXPECT_EQ(cancelled, 1) << "the one started action is cancelled once, when the loop ends";
    for (const auto &name : names)
    {
        EXPECT_FALSE((*gameplay)[name].IsActive()) << name << " was left active in the map that was left";
    }

    _input.Update();
    EXPECT_EQ(started, 1) << "the map left is no longer updated";
    EXPECT_EQ(cancelled, 1);
}

// ============================================================================
// Keyboard and mouse bindings without a window
// ============================================================================

// Note: GLFW is not initialized in the test binary, and an uninitialized GLFW returns early from
// glfwGetKey/glfwGetMouseButton (in Release, where its null-window assert is compiled out), so these pass
// with or without the guard there. They pin the result; the guard is what keeps a null window from reaching
// GLFW once it is initialized. There is no seam to observe "no GLFW call" without GLFW itself.
TEST(NullWindowBindingTest, KeyboardBindingReadsReleased)
{
    KeyboardButtonBinding binding(static_cast<GLFWwindow *>(nullptr), Key::Space);
    EXPECT_FALSE(binding.getValue().asBool());
}

TEST(NullWindowBindingTest, MouseBindingReadsReleased)
{
    MouseButtonBinding binding(static_cast<GLFWwindow *>(nullptr), MouseButton::Left);
    EXPECT_FALSE(binding.getValue().asBool());
}

TEST(NullWindowBindingTest, CompositeBindingReadsZero)
{
    Vector2CompositeBinding binding(static_cast<GLFWwindow *>(nullptr), Key::W, Key::S, Key::A, Key::D);
    const InputValue value = binding.getValue();
    EXPECT_TRUE(value.Is<Math::Vector2>());
    EXPECT_FLOAT_EQ(value.asVector2().x, 0.0f);
    EXPECT_FLOAT_EQ(value.asVector2().y, 0.0f);
}

TEST_F(InputMapSwitchCancelTest, HeadlessSystemUpdatesKeyboardAndMouseActions)
{
    // Built from JSON on a Window that never opened: the bindings hold a null GLFW window
    const json map = {{"actions", {
        {"Jump", {{"bindings", json::array({{{"type", "KeyboardButton"}, {"key", "Space"}}})}}},
        {"Click", {{"bindings", json::array({{{"type", "MouseButton"}, {"button", "Left"}}})}}},
    }}};
    ActionMap *created = _input.CreateActionMapFromJson("Headless", map);
    ASSERT_NE(created, nullptr);
    _input.LoadActionMap("Headless");

    EXPECT_NO_FATAL_FAILURE(_input.Update());
    EXPECT_FALSE((*created)["Jump"].IsActive());
    EXPECT_FALSE((*created)["Click"].IsActive());
}
