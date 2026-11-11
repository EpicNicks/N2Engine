#include <gtest/gtest.h>

#include <memory>
#include <set>

#include <nlohmann/json.hpp>

#include <engine/input/ActionMap.hpp>
#include <engine/input/InputBinding.hpp>
#include <engine/input/InputDevice.hpp>
#include <engine/input/Keyboard.hpp>
#include <engine/input/Mouse.hpp>

#include "input/GlfwInputMapping.hpp" // engine/src: the GLFW device's private tables

// Engine input codes (build targets stage 5; docs/input.html#devices): the input system reads an InputDevice in engine
// codes, and only the GLFW device knows GLFW's numbers. Its tables are total both ways, every binding and the mouse and
// keyboard read through a device, and saved bindings (which name their keys) load as they did.
using namespace N2Engine;
using namespace N2Engine::Input;

namespace
{
    /// A device whose state the test sets
    class FakeDevice final : public InputDevice
    {
    public:
        std::set<Key> keys;
        std::set<MouseButton> buttons;
        Math::Vector2 cursor{10.0f, 20.0f};
        bool gamepadConnected = false;
        GamepadState gamepad;
        bool sticky = false;
        std::function<void(Math::Vector2)> scroll;
        std::function<void(unsigned int)> text;

        bool IsKeyDown(const Key key) const override { return keys.contains(key); }
        bool IsMouseButtonDown(const MouseButton button) const override { return buttons.contains(button); }
        std::optional<Math::Vector2> CursorPosition() const override { return cursor; }
        bool GetGamepadState(const int id, GamepadState &state) const override
        {
            if (id != 0 || !gamepadConnected)
            {
                return false;
            }
            state = gamepad;
            return true;
        }
        bool IsJoystickPresent(const int id) const override { return id == 0 && gamepadConnected; }
        bool IsGamepad(const int id) const override { return IsJoystickPresent(id); }
        std::string GamepadName(const int id) const override { return IsJoystickPresent(id) ? "Fake Pad" : ""; }
        void SetStickyInput(const bool on) override { sticky = on; }
        void SetScrollHandler(std::function<void(Math::Vector2)> handler) override { scroll = std::move(handler); }
        void SetTextHandler(std::function<void(unsigned int)> handler) override { text = std::move(handler); }
    };
}

TEST(InputDeviceTest, TheGlfwTablesAreTotalBothWays)
{
    for (std::size_t i = 0; i <= static_cast<std::size_t>(Key::Menu); ++i)
    {
        const auto key = static_cast<Key>(i);
        ASSERT_TRUE(KeyToGLFW.contains(key)) << i;
        EXPECT_EQ(GLFWToKey.at(KeyToGLFW.at(key)), key) << i;
    }
    EXPECT_EQ(GLFWToKey.size(), KeyToGLFW.size()) << "no two engine keys share a GLFW key";
    for (std::size_t i = 0; i < GamepadButtonCount; ++i)
    {
        const auto button = static_cast<GamepadButton>(i);
        ASSERT_TRUE(GamepadButtonToGLFW.contains(button)) << i;
        EXPECT_EQ(GLFWToGamepadButton.at(GamepadButtonToGLFW.at(button)), button);
    }
    for (std::size_t i = 0; i < GamepadAxisCount; ++i)
    {
        EXPECT_TRUE(GamepadAxisToGLFW.contains(static_cast<GamepadAxis>(i))) << i;
    }
    for (int i = 0; i < Mouse::ButtonCount; ++i)
    {
        const auto button = static_cast<MouseButton>(i);
        ASSERT_TRUE(MouseButtonToGLFW.contains(button)) << i;
        EXPECT_EQ(GLFWToMouseButton.at(MouseButtonToGLFW.at(button)), button);
    }
}

TEST(InputDeviceTest, BindingsReadTheirDevice)
{
    FakeDevice device;
    KeyboardButtonBinding space(&device, Key::Space);
    MouseButtonBinding left(&device, MouseButton::Left);
    Vector2CompositeBinding wasd(&device, Key::W, Key::S, Key::A, Key::D);
    GamepadButtonBinding south(&device, GamepadButton::South);
    AxisBinding trigger(&device, GamepadAxis::RightTrigger);
    GamepadButtonCompositeBinding dpad(&device, GamepadButton::DpadUp, GamepadButton::DpadDown,
                                       GamepadButton::DpadLeft, GamepadButton::DpadRight);

    EXPECT_FALSE(space.getValue().asBool());
    EXPECT_FALSE(south.getValue().asBool()) << "no gamepad connected";
    device.keys = {Key::Space, Key::W, Key::D};
    device.buttons = {MouseButton::Left};
    EXPECT_TRUE(space.getValue().asBool());
    EXPECT_TRUE(left.getValue().asBool());
    EXPECT_EQ(wasd.getValue().asVector2(), Math::Vector2(1.0f, 1.0f));

    device.gamepadConnected = true;
    device.gamepad.buttons[static_cast<std::size_t>(GamepadButton::South)] = true;
    device.gamepad.buttons[static_cast<std::size_t>(GamepadButton::DpadLeft)] = true;
    device.gamepad.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] = 1.0f; // fully pressed
    EXPECT_TRUE(south.getValue().asBool());
    EXPECT_FLOAT_EQ(trigger.getValue().asFloat(), 1.0f);
    EXPECT_EQ(dpad.getValue().asVector2(), Math::Vector2(-1.0f, 0.0f));

    KeyboardButtonBinding none(nullptr, Key::Space);
    EXPECT_FALSE(none.getValue().asBool()) << "no device: nothing is pressed";
}

TEST(InputDeviceTest, MouseAndKeyboardReadTheirDevice)
{
    FakeDevice device;
    {
        Mouse mouse(&device);
        Keyboard keyboard(&device);
        EXPECT_TRUE(device.sticky);
        ASSERT_TRUE(device.scroll && device.text);

        device.cursor = {100.0f, 50.0f};
        device.buttons = {MouseButton::Right};
        device.scroll(Math::Vector2(0.0f, 2.0f));
        device.keys = {Key::Enter};
        device.text(U'é');
        mouse.Update();
        keyboard.Update();
        EXPECT_EQ(mouse.GetPosition(), Math::Vector2(100.0f, 50.0f));
        EXPECT_TRUE(mouse.GetButtonDown(MouseButton::Right));
        EXPECT_EQ(mouse.GetScrollDelta(), Math::Vector2(0.0f, 2.0f));
        EXPECT_TRUE(keyboard.GetKeyDown(Key::Enter));
        EXPECT_EQ(keyboard.GetTypedText(), "\xc3\xa9");
    }
    EXPECT_FALSE(device.scroll) << "the mouse dropped its handler";
    EXPECT_FALSE(device.text) << "the keyboard dropped its handler";
}

TEST(InputDeviceTest, AnActionMapSavedBeforeThisStageLoadsUnchanged)
{
    // Bindings name their keys, buttons and axes ("W", "Left", "South"): the same file loads, and saves back the same
    const nlohmann::json saved = nlohmann::json::parse(R"({
        "actions": {
            "Move": {"bindings": [{"type": "Vector2Composite", "up": "W", "down": "S", "left": "A", "right": "D"}]},
            "Jump": {"bindings": [{"type": "KeyboardButton", "key": "Space"},
                                  {"type": "GamepadButton", "button": "South"}]},
            "Fire": {"bindings": [{"type": "MouseButton", "button": "Left"},
                                  {"type": "GamepadAxis", "axis": "RightTrigger", "pressPoint": 0.5}]}
        }
    })");
    auto map = ActionMap::Deserialize(saved, "Gameplay", nullptr);
    ASSERT_TRUE(map.has_value());
    auto again = ActionMap::Deserialize((*map)->Serialize(), "Gameplay", nullptr);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ((*again)->Serialize(), (*map)->Serialize());
    const std::string text = (*map)->Serialize().dump();
    for (const char *name : {"\"W\"", "\"Space\"", "\"South\"", "\"Left\"", "\"RightTrigger\""})
    {
        EXPECT_NE(text.find(name), std::string::npos) << name;
    }
}
