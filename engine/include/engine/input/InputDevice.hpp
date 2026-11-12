#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include <math/Vector2.hpp>

#include "engine/input/InputTypes.hpp"

namespace N2Engine::Input
{
    /// How many of each the engine knows (InputTypes.hpp)
    inline constexpr std::size_t GamepadButtonCount = static_cast<std::size_t>(GamepadButton::DpadLeft) + 1;
    inline constexpr std::size_t GamepadAxisCount = static_cast<std::size_t>(GamepadAxis::RightTrigger) + 1;
    /// The most gamepads a device reports (GLFW's joystick slots)
    inline constexpr int MaxGamepads = 16;

    /// A gamepad as it is now, in engine codes: buttons by GamepadButton, axes by GamepadAxis. Sticks are -1..1 (y down is
    /// +1, as GLFW reports it); triggers rest at -1 and are +1 fully pressed (InputBinding maps them to 0..1).
    struct GamepadState
    {
        std::array<bool, GamepadButtonCount> buttons{};
        std::array<float, GamepadAxisCount> axes{};

        [[nodiscard]] bool Button(GamepadButton button) const { return buttons[static_cast<std::size_t>(button)]; }
        [[nodiscard]] float Axis(GamepadAxis axis) const { return axes[static_cast<std::size_t>(axis)]; }
    };

    /**
     * Where the input system reads the devices from (build targets stage 5; docs/input.html#devices): keys, mouse buttons,
     * the cursor, scroll, typed text and gamepads, all in engine codes. The desktop's is GLFW's (GlfwInputDevice, the only
     * code that knows GLFW's key, button and axis numbers); later platforms bring their own through the platform layer.
     * Without a window there is no device: nothing is pressed, and hosts inject input instead (KeySource, Mouse's
     * InjectPointer, Touchscreen).
     */
    class InputDevice
    {
    public:
        virtual ~InputDevice() = default;

        /// Whether the key is held. Key::Unknown never is.
        [[nodiscard]] virtual bool IsKeyDown(Key key) const = 0;
        [[nodiscard]] virtual bool IsMouseButtonDown(MouseButton button) const = 0;
        /// The cursor in window coordinates (top-left origin, y down), or nullopt when the device can't tell
        [[nodiscard]] virtual std::optional<Math::Vector2> CursorPosition() const = 0;

        /// Whether a gamepad is connected at slot id, and if so its state
        [[nodiscard]] virtual bool GetGamepadState(int id, GamepadState &state) const = 0;
        /// A joystick at slot id (a gamepad or not), and whether it has a gamepad mapping
        [[nodiscard]] virtual bool IsJoystickPresent(int id) const = 0;
        [[nodiscard]] virtual bool IsGamepad(int id) const = 0;
        /// The device's name for a joystick or gamepad slot ("" when there is none)
        [[nodiscard]] virtual std::string GamepadName(int id) const = 0;

        /// Sticky keys and mouse buttons: a press shorter than a frame is still seen once (GLFW's sticky input modes)
        virtual void SetStickyInput(bool sticky) = 0;
        /// Called with each scroll (x, y), as it arrives; an empty function stops it
        virtual void SetScrollHandler(std::function<void(Math::Vector2)> handler) = 0;
        /// Called with each typed Unicode code point, as it arrives; an empty function stops it
        virtual void SetTextHandler(std::function<void(unsigned int)> handler) = 0;
    };
}
