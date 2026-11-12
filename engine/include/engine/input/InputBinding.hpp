#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <math/Vector2.hpp>
#include "engine/input/InputTypes.hpp"

namespace N2Engine::Input
{
    class InputDevice;
}

namespace N2Engine
{
    class Window;
}

namespace N2Engine::Input
{
    class InputValue;

    class InputBinding
    {
    protected:
        /// The device read (build targets stage 5; null: none, so nothing is pressed)
        InputDevice *window;

    public:
        explicit InputBinding(InputDevice *win) : window(win) {}
        explicit InputBinding(const Window &win);
        virtual ~InputBinding() = default;

        virtual InputValue getValue() = 0;
        [[nodiscard]] virtual BindingType GetType() const = 0;
        [[nodiscard]] virtual nlohmann::json Serialize() const = 0;
    };

    class KeyboardButtonBinding : public InputBinding
    {
        Key boundKey;

    public:
        KeyboardButtonBinding(InputDevice *win, Key key) : InputBinding(win), boundKey(key) {}
        KeyboardButtonBinding(const Window &win, Key key) : InputBinding(win), boundKey(key) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::KeyboardButton; }
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    class AxisBinding : public InputBinding
    {
        GamepadAxis boundAxis;
        int gamepadId;
        AxisDirection direction = AxisDirection::Both;
        float pressPoint = 0.0f; // > 0: the binding is a button, pressed at or past this magnitude

    public:
        AxisBinding(InputDevice *win, GamepadAxis axis, const int joyId = 0,
                    const AxisDirection direction = AxisDirection::Both, const float pressPoint = 0.0f)
            : InputBinding(win), boundAxis(axis), gamepadId(joyId), direction(direction), pressPoint(pressPoint) {}

        AxisBinding(const Window &win, GamepadAxis axis, const int joyId = 0,
                    const AxisDirection direction = AxisDirection::Both, const float pressPoint = 0.0f)
            : InputBinding(win), boundAxis(axis), gamepadId(joyId), direction(direction), pressPoint(pressPoint) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::GamepadAxis; }

        static constexpr float Deadzone = 0.1f;
        /// Maps a raw GLFW axis reading to the binding's value. Triggers rest at -1 in GLFW; they're
        /// mapped to 0..1 so a released trigger reads 0 (it used to count as held forever). Readings
        /// within the deadzone become 0, and the rest is rescaled to keep the full range.
        static float NormalizeAxis(GamepadAxis axis, float raw);
        /// Applies the direction and press point to a normalized axis value: Positive or Negative keep that
        /// half as a 0..1 magnitude (the other half reads 0); a press point above 0 turns the result into a
        /// bool that is true at or past it (Unity's default press point is 0.5)
        static InputValue Shape(float normalized, AxisDirection direction, float pressPoint);
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    /// Four gamepad buttons read as a Vector2, as Vector2CompositeBinding reads four keys (the D-pad, or the
    /// face buttons used as a second stick)
    class GamepadButtonCompositeBinding : public InputBinding
    {
        GamepadButton up, down, left, right;
        int gamepadId;

    public:
        GamepadButtonCompositeBinding(InputDevice *win, GamepadButton upButton, GamepadButton downButton,
                                      GamepadButton leftButton, GamepadButton rightButton, const int joyId = 0)
            : InputBinding(win), up(upButton), down(downButton), left(leftButton), right(rightButton),
              gamepadId(joyId) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::GamepadButtonComposite; }
        /// The composite's value from the four button states: +1 right, -1 left, +1 up, -1 down, so opposite
        /// buttons cancel (as Vector2CompositeBinding does; a diagonal reads (1, 1), the action normalizes it)
        static Math::Vector2 Combine(bool upDown, bool downDown, bool leftDown, bool rightDown);
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    /// A binding that counts only while a modifier binding is held (Unity's "One Modifier" composite, such
    /// as a stick click that needs the other stick clicked too). Its value is the main binding's value while
    /// the modifier reads as pressed, else that value's zero (false, 0 or (0, 0)). The order the two are
    /// pressed in does not matter.
    class ChordBinding : public InputBinding
    {
        std::unique_ptr<InputBinding> modifier;
        std::unique_ptr<InputBinding> binding;

    public:
        ChordBinding(InputDevice *win, std::unique_ptr<InputBinding> modifierBinding,
                     std::unique_ptr<InputBinding> mainBinding)
            : InputBinding(win), modifier(std::move(modifierBinding)), binding(std::move(mainBinding)) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::Chord; }
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    class GamepadStickBinding : public InputBinding
    {
        GamepadAxis xAxis;
        GamepadAxis yAxis;
        int gamepadId;
        float deadzone;
        bool invertXAxis;
        bool invertYAxis;

    public:
        GamepadStickBinding(InputDevice *win, GamepadAxis xAxis, GamepadAxis yAxis, int joyId = 0,
                            float deadzone = 0.15f, bool invertX = false, bool invertY = false)
            : InputBinding(win), xAxis(xAxis), yAxis(yAxis), gamepadId(joyId), deadzone(deadzone),
              invertXAxis(invertX), invertYAxis(invertY) {}

        GamepadStickBinding(const Window &win, GamepadAxis xAxis, GamepadAxis yAxis, int joyId = 0,
                            float deadzone = 0.15f, bool invertX = false, bool invertY = false)
            : InputBinding(win), xAxis(xAxis), yAxis(yAxis), gamepadId(joyId), deadzone(deadzone),
              invertXAxis(invertX), invertYAxis(invertY) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::GamepadStick; }
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    class Vector2CompositeBinding : public InputBinding
    {
        Key up, down, left, right;

    public:
        Vector2CompositeBinding(InputDevice *win, Key upKey, Key downKey, Key leftKey, Key rightKey)
            : InputBinding(win), up(upKey), down(downKey), left(leftKey), right(rightKey) {}

        Vector2CompositeBinding(const Window &window, Key upKey, Key downKey, Key leftKey, Key rightKey)
            : InputBinding(window), up(upKey), down(downKey), left(leftKey), right(rightKey) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::Vector2Composite; }
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    class MouseButtonBinding : public InputBinding
    {
        MouseButton boundButton;

    public:
        MouseButtonBinding(InputDevice *win, MouseButton button)
            : InputBinding(win), boundButton(button) {}

        MouseButtonBinding(const Window &win, MouseButton button)
            : InputBinding(win), boundButton(button) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::MouseButton; }
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    class MouseWheelBinding : public InputBinding
    {

    };

    class GamepadButtonBinding : public InputBinding
    {
        GamepadButton boundButton;
        int gamepadId;

    public:
        GamepadButtonBinding(InputDevice *win, GamepadButton button, const int joyId = 0)
            : InputBinding(win), boundButton(button), gamepadId(joyId) {}

        GamepadButtonBinding(const Window &win, GamepadButton button, const int joyId = 0)
            : InputBinding(win), boundButton(button), gamepadId(joyId) {}

        InputValue getValue() override;
        [[nodiscard]] nlohmann::json Serialize() const override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::GamepadButton; }
    };
}
