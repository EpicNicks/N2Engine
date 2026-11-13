#include <gtest/gtest.h>

#include <engine/input/GamepadSource.hpp>
#include <engine/input/InputBinding.hpp>
#include <engine/input/InputSystem.hpp>
#include <engine/input/InputValue.hpp>

// GamepadSource (docs/input.html#gamepad-source): a host that reads gamepads itself (the web player) installs one, and
// every gamepad read (the bindings, GetConnectedGamepads) asks it instead of the window's device.
using namespace N2Engine;
using namespace N2Engine::Input;

namespace
{
    /// Installs a source for one test and takes it out again, so no other test sees it
    struct ScopedSource
    {
        explicit ScopedSource(const GamepadSource *source) : previous(GamepadSource::Set(source)) {}
        ~ScopedSource() { GamepadSource::Set(previous); }
        const GamepadSource *previous;
    };

    GamepadState Pressed(const GamepadButton button)
    {
        GamepadState state;
        state.buttons[static_cast<std::size_t>(button)] = true;
        state.axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] = -1.0f;
        state.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] = -1.0f;
        return state;
    }
}

TEST(GamepadSourceTest, InjectedGamepadsHoldWhatTheyWereTold)
{
    InjectedGamepads pads;
    GamepadState state;
    EXPECT_FALSE(pads.IsJoystickPresent(0));
    EXPECT_FALSE(pads.GetGamepadState(0, state));
    EXPECT_EQ(pads.GamepadName(0), "");

    pads.Connect(1, "Pad One", Pressed(GamepadButton::East));
    EXPECT_TRUE(pads.IsJoystickPresent(1));
    EXPECT_TRUE(pads.IsGamepad(1));
    EXPECT_EQ(pads.GamepadName(1), "Pad One");
    ASSERT_TRUE(pads.GetGamepadState(1, state));
    EXPECT_TRUE(state.Button(GamepadButton::East));
    EXPECT_FALSE(pads.IsJoystickPresent(0)) << "slots are independent";

    pads.Connect(2, "Wheel", std::nullopt);
    EXPECT_TRUE(pads.IsJoystickPresent(2));
    EXPECT_FALSE(pads.IsGamepad(2)) << "no mapping: a joystick, not a gamepad";
    EXPECT_FALSE(pads.GetGamepadState(2, state));

    pads.Connect(-1, "out of range", Pressed(GamepadButton::South));
    pads.Connect(MaxGamepads, "out of range", Pressed(GamepadButton::South));
    EXPECT_FALSE(pads.IsJoystickPresent(-1));
    EXPECT_FALSE(pads.IsJoystickPresent(MaxGamepads));

    pads.Disconnect(1);
    EXPECT_FALSE(pads.IsJoystickPresent(1));
    pads.DisconnectAll();
    EXPECT_FALSE(pads.IsJoystickPresent(2));
}

TEST(GamepadSourceTest, BindingsWithNoDeviceReadTheInstalledSource)
{
    // Windowless, as the web player runs: no device, so without a source nothing reads
    GamepadButtonBinding south(nullptr, GamepadButton::South);
    AxisBinding trigger(nullptr, GamepadAxis::RightTrigger);
    GamepadStickBinding stick(nullptr, GamepadAxis::LeftX, GamepadAxis::LeftY);
    GamepadButtonCompositeBinding dpad(nullptr, GamepadButton::DpadUp, GamepadButton::DpadDown,
                                       GamepadButton::DpadLeft, GamepadButton::DpadRight);
    GamepadButtonBinding secondPad(nullptr, GamepadButton::South, 1);
    EXPECT_FALSE(south.getValue().asBool());

    InjectedGamepads pads;
    ScopedSource installed(&pads);
    EXPECT_FALSE(south.getValue().asBool()) << "a source with no pad";

    GamepadState state = Pressed(GamepadButton::South);
    state.buttons[static_cast<std::size_t>(GamepadButton::DpadRight)] = true;
    state.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] = 1.0f;
    state.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = 1.0f;
    pads.Connect(0, "Pad", state);
    EXPECT_TRUE(south.getValue().asBool());
    EXPECT_FLOAT_EQ(trigger.getValue().asFloat(), 1.0f);
    EXPECT_FLOAT_EQ(stick.getValue().asVector2().x, 1.0f);
    EXPECT_EQ(dpad.getValue().asVector2(), Math::Vector2(1.0f, 0.0f));
    EXPECT_FALSE(secondPad.getValue().asBool()) << "pad 1 is not there";

    pads.Disconnect(0);
    EXPECT_FALSE(south.getValue().asBool()) << "an unplugged pad reads released";
}

TEST(GamepadSourceTest, TheSourceReplacesABindingsOwnDevice)
{
    // A device that always reports South held on pad 0
    class HeldSouth final : public InputDevice
    {
    public:
        bool IsKeyDown(Key) const override { return false; }
        bool IsMouseButtonDown(MouseButton) const override { return false; }
        std::optional<Math::Vector2> CursorPosition() const override { return std::nullopt; }
        bool GetGamepadState(const int id, GamepadState &state) const override
        {
            state = Pressed(GamepadButton::South);
            return id == 0;
        }
        bool IsJoystickPresent(const int id) const override { return id == 0; }
        bool IsGamepad(const int id) const override { return id == 0; }
        std::string GamepadName(const int id) const override { return id == 0 ? "Device Pad" : ""; }
        void SetStickyInput(bool) override {}
        void SetScrollHandler(std::function<void(Math::Vector2)>) override {}
        void SetTextHandler(std::function<void(unsigned int)>) override {}
    } device;

    GamepadButtonBinding south(&device, GamepadButton::South);
    EXPECT_TRUE(south.getValue().asBool());

    InjectedGamepads pads;
    ScopedSource installed(&pads);
    EXPECT_FALSE(south.getValue().asBool()) << "while a source is installed the device is never asked";
}

TEST(GamepadSourceTest, ConnectedGamepadsComeFromTheSource)
{
    EXPECT_TRUE(InputSystem::GetConnectedGamepads().empty()) << "no window, no source";

    InjectedGamepads pads;
    ScopedSource installed(&pads);
    pads.Connect(0, "Xbox Controller", Pressed(GamepadButton::South));
    pads.Connect(3, "Odd Stick", std::nullopt);
    const auto connected = InputSystem::GetConnectedGamepads();
    ASSERT_EQ(connected.size(), 2u);
    EXPECT_EQ(connected[0].gamepadId, 0);
    EXPECT_EQ(connected[0].name, "Xbox Controller");
    EXPECT_EQ(connected[1].gamepadId, 3);
    EXPECT_EQ(connected[1].name, "Unrecognized Gamepad Mapping: Odd Stick");
}
