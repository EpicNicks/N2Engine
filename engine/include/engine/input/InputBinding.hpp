#pragma once

class GLFWwindow;

namespace N2Engine
{
    class Window;

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
            InputBinding(GLFWwindow *win) : window(win) {}
            InputBinding(Window &win);
            virtual InputValue getValue() = 0;
        };

        class ButtonBinding : public InputBinding
        {
            Key boundKey;

        public:
            ButtonBinding(GLFWwindow *win, Key key) : InputBinding(win), boundKey(key) {}
            ButtonBinding(Window &win, Key key) : InputBinding(win), boundKey(key) {}

            InputValue getValue() override;
        };

        class AxisBinding : public InputBinding
        {
            GamepadAxis boundAxis;
            int gamepadId;

        public:
            AxisBinding(GLFWwindow *win, GamepadAxis axis, int joyId = 0)
                : InputBinding(win), boundAxis(axis), gamepadId(joyId) {}
            AxisBinding(Window &win, GamepadAxis axis, int joyId = 0)
                : InputBinding(win), boundAxis(axis), gamepadId(joyId) {}

            InputValue getValue() override;
        };

        class GamepadStickBinding : public InputBinding
        {
            GamepadAxis xAxis;
            GamepadAxis yAxis;
            int gamepadId;
            float deadzone;

        public:
            GamepadStickBinding(GLFWwindow *win, GamepadAxis xAxis, GamepadAxis yAxis, int joyId = 0, float deadzone = 0.15f)
                : InputBinding(win), xAxis(xAxis), yAxis(yAxis), gamepadId(joyId), deadzone(deadzone) {}

            GamepadStickBinding(Window &win, GamepadAxis xAxis, GamepadAxis yAxis, int joyId = 0, float deadzone = 0.15f)
                : InputBinding(win), xAxis(xAxis), yAxis(yAxis), gamepadId(joyId), deadzone(deadzone) {}

            InputValue getValue() override;
        };

        class Vector2CompositeBinding : public InputBinding
        {
            Key up, down, left, right;

        public:
            Vector2CompositeBinding(GLFWwindow *win, Key upKey, Key downKey, Key leftKey, Key rightKey)
                : InputBinding(win), up(upKey), down(downKey), left(leftKey), right(rightKey) {}
            Vector2CompositeBinding(Window &window, Key upKey, Key downKey, Key leftKey, Key rightKey)
                : InputBinding(window), up(upKey), down(downKey), left(leftKey), right(rightKey) {}

            InputValue getValue() override;
        };

        class MouseButtonBinding : public InputBinding
        {
            MouseButton boundButton;

        public:
            MouseButtonBinding(GLFWwindow *win, MouseButton button)
                : InputBinding(win), boundButton(button) {}
            MouseButtonBinding(Window &win, MouseButton button)
                : InputBinding(win), boundButton(button) {}

            InputValue getValue() override;
        };

        class GamepadButtonBinding : public InputBinding
        {
            GamepadButton boundButton;
            int gamepadId;

        public:
            GamepadButtonBinding(GLFWwindow *win, GamepadButton button, int joyId = 0)
                : InputBinding(win), boundButton(button), gamepadId(joyId) {}
            GamepadButtonBinding(Window &win, GamepadButton button, int joyId = 0)
                : InputBinding(win), boundButton(button), gamepadId(joyId) {}

            InputValue getValue() override;
        };
    }
}