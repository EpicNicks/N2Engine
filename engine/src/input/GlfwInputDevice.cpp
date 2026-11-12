#include "GlfwInputDevice.hpp"

#include <mutex>
#include <unordered_map>

#include "GlfwInputMapping.hpp"

namespace N2Engine::Input
{
    namespace
    {
        class GlfwInputDevice;

        /// GLFW's callbacks carry only the window: each finds its device here (one window today, any number allowed)
        std::unordered_map<GLFWwindow *, GlfwInputDevice *> &Devices()
        {
            static std::unordered_map<GLFWwindow *, GlfwInputDevice *> devices;
            return devices;
        }

        class GlfwInputDevice final : public InputDevice
        {
        public:
            explicit GlfwInputDevice(GLFWwindow *window) : _window(window)
            {
                Devices()[_window] = this;
                glfwSetScrollCallback(_window, ScrollCallback);
                glfwSetCharCallback(_window, CharCallback);
            }

            ~GlfwInputDevice() override
            {
                glfwSetScrollCallback(_window, nullptr);
                glfwSetCharCallback(_window, nullptr);
                SetStickyInput(false);
                Devices().erase(_window);
            }

            GlfwInputDevice(const GlfwInputDevice &) = delete;
            GlfwInputDevice &operator=(const GlfwInputDevice &) = delete;

            bool IsKeyDown(const Key key) const override
            {
                if (key == Key::Unknown)
                {
                    return false;
                }
                const auto it = KeyToGLFW.find(key);
                return it != KeyToGLFW.end() && glfwGetKey(_window, it->second) == GLFW_PRESS;
            }

            bool IsMouseButtonDown(const MouseButton button) const override
            {
                const auto it = MouseButtonToGLFW.find(button);
                return it != MouseButtonToGLFW.end() && glfwGetMouseButton(_window, it->second) == GLFW_PRESS;
            }

            std::optional<Math::Vector2> CursorPosition() const override
            {
                double x = 0.0, y = 0.0;
                glfwGetCursorPos(_window, &x, &y);
                return Math::Vector2(static_cast<float>(x), static_cast<float>(y));
            }

            bool GetGamepadState(const int id, GamepadState &state) const override
            {
                GLFWgamepadstate raw;
                if (id < 0 || id >= MaxGamepads || !glfwGetGamepadState(id, &raw))
                {
                    return false;
                }
                for (const auto &[button, glfwButton] : GamepadButtonToGLFW)
                {
                    state.buttons[static_cast<std::size_t>(button)] = raw.buttons[glfwButton] == GLFW_PRESS;
                }
                for (const auto &[axis, glfwAxis] : GamepadAxisToGLFW)
                {
                    state.axes[static_cast<std::size_t>(axis)] = raw.axes[glfwAxis];
                }
                return true;
            }

            bool IsJoystickPresent(const int id) const override
            {
                return id >= 0 && id < MaxGamepads && glfwJoystickPresent(id) == GLFW_TRUE;
            }

            bool IsGamepad(const int id) const override
            {
                return id >= 0 && id < MaxGamepads && glfwJoystickIsGamepad(id) == GLFW_TRUE;
            }

            std::string GamepadName(const int id) const override
            {
                if (!IsJoystickPresent(id))
                {
                    return {};
                }
                const char *name = IsGamepad(id) ? glfwGetGamepadName(id) : glfwGetJoystickName(id);
                return name ? std::string(name) : std::string();
            }

            void SetStickyInput(const bool sticky) override
            {
                glfwSetInputMode(_window, GLFW_STICKY_KEYS, sticky ? GLFW_TRUE : GLFW_FALSE);
                glfwSetInputMode(_window, GLFW_STICKY_MOUSE_BUTTONS, sticky ? GLFW_TRUE : GLFW_FALSE);
            }

            void SetScrollHandler(std::function<void(Math::Vector2)> handler) override { _scroll = std::move(handler); }
            void SetTextHandler(std::function<void(unsigned int)> handler) override { _text = std::move(handler); }

        private:
            static GlfwInputDevice *Of(GLFWwindow *window)
            {
                const auto it = Devices().find(window);
                return it != Devices().end() ? it->second : nullptr;
            }

            static void ScrollCallback(GLFWwindow *window, const double x, const double y)
            {
                if (auto *device = Of(window); device && device->_scroll)
                {
                    device->_scroll(Math::Vector2(static_cast<float>(x), static_cast<float>(y)));
                }
            }

            static void CharCallback(GLFWwindow *window, const unsigned int codepoint)
            {
                if (auto *device = Of(window); device && device->_text)
                {
                    device->_text(codepoint);
                }
            }

            GLFWwindow *_window;
            std::function<void(Math::Vector2)> _scroll;
            std::function<void(unsigned int)> _text;
        };
    }

    std::unique_ptr<InputDevice> MakeGlfwInputDevice(GLFWwindow *window)
    {
        if (window == nullptr)
        {
            return nullptr;
        }
        return std::make_unique<GlfwInputDevice>(window);
    }
}
