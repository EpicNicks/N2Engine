#pragma once
#include <nlohmann/json.hpp>

class GLFWwindow;

namespace N2Engine
{
    class Window;

    enum class BindingType
    {
        KeyboardButton,
        GamepadButton,
        GamepadAxis,
        GamepadStick,
        Vector2Composite,
        MouseButton
    };

    NLOHMANN_JSON_SERIALIZE_ENUM(BindingType, {
                                 {BindingType::KeyboardButton, "KeyboardButton"},
                                 {BindingType::GamepadButton, "GamepadButton"},
                                 {BindingType::GamepadAxis, "GamepadAxis"},
                                 {BindingType::GamepadStick, "GamepadStick"},
                                 {BindingType::Vector2Composite, "Vector2Composite"},
                                 {BindingType::MouseButton, "MouseButton"}
                                 })

    namespace Input
    {
        enum class Key;
        enum class GamepadAxis;
        enum class MouseButton;
        enum class GamepadButton;
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
            virtual BindingType GetType() const = 0;
            virtual nlohmann::json Serialize() const = 0;
        };

        class KeyboardButtonBinding : public InputBinding
        {
            Key boundKey;

        public:
            KeyboardButtonBinding(GLFWwindow *win, Key key) : InputBinding(win), boundKey(key) {}
            KeyboardButtonBinding(const Window &win, Key key) : InputBinding(win), boundKey(key) {}

            InputValue getValue() override;
            BindingType GetType() const override { return BindingType::KeyboardButton; }
            nlohmann::json Serialize() const override;
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
            BindingType GetType() const override { return BindingType::GamepadAxis; }
            nlohmann::json Serialize() const override;
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

            GamepadStickBinding(Window &win, GamepadAxis xAxis, GamepadAxis yAxis, int joyId = 0,
                                float deadzone = 0.15f, bool invertX = false, bool invertY = false)
                : InputBinding(win), xAxis(xAxis), yAxis(yAxis), gamepadId(joyId), deadzone(deadzone),
                  invertXAxis(invertX), invertYAxis(invertY) {}

            InputValue getValue() override;
            BindingType GetType() const override { return BindingType::GamepadStick; }
            nlohmann::json Serialize() const override;
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
            BindingType GetType() const override { return BindingType::Vector2Composite; }
            nlohmann::json Serialize() const override;
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
            BindingType GetType() const override { return BindingType::MouseButton; }
            nlohmann::json Serialize() const override;
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

            nlohmann::json Serialize() const override;
            BindingType GetType() const override { return BindingType::GamepadButton; }
            InputValue getValue() override;
        };
    }
}
