#pragma once
#include <nlohmann/json.hpp>
#include "engine/input/InputTypes.hpp"

struct GLFWwindow;

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
        GLFWwindow *window; // Need window handle for polling

    public:
        explicit InputBinding(GLFWwindow *win) : window(win) {}
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
        KeyboardButtonBinding(GLFWwindow *win, Key key) : InputBinding(win), boundKey(key) {}
        KeyboardButtonBinding(const Window &win, Key key) : InputBinding(win), boundKey(key) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::KeyboardButton; }
        [[nodiscard]] nlohmann::json Serialize() const override;
    };

    class AxisBinding : public InputBinding
    {
        GamepadAxis boundAxis;
        int gamepadId;

    public:
        AxisBinding(GLFWwindow *win, GamepadAxis axis, const int joyId = 0)
            : InputBinding(win), boundAxis(axis), gamepadId(joyId) {}

        AxisBinding(const Window &win, GamepadAxis axis, const int joyId = 0)
            : InputBinding(win), boundAxis(axis), gamepadId(joyId) {}

        InputValue getValue() override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::GamepadAxis; }
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
        GamepadStickBinding(GLFWwindow *win, GamepadAxis xAxis, GamepadAxis yAxis, int joyId = 0,
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
        Vector2CompositeBinding(GLFWwindow *win, Key upKey, Key downKey, Key leftKey, Key rightKey)
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
        MouseButtonBinding(GLFWwindow *win, MouseButton button)
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
        GamepadButtonBinding(GLFWwindow *win, GamepadButton button, const int joyId = 0)
            : InputBinding(win), boundButton(button), gamepadId(joyId) {}

        GamepadButtonBinding(const Window &win, GamepadButton button, const int joyId = 0)
            : InputBinding(win), boundButton(button), gamepadId(joyId) {}

        InputValue getValue() override;
        [[nodiscard]] nlohmann::json Serialize() const override;
        [[nodiscard]] BindingType GetType() const override { return BindingType::GamepadButton; }
    };
}
