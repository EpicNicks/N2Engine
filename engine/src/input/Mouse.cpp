#include "engine/input/Mouse.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/Application.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/InputTypes.hpp"

namespace N2Engine::Input
{
    Mouse::Mouse(GLFWwindow *window)
        : _window(window)
    {
        if (!_window)
        {
            return; // no window (e.g. headless): position and buttons stay as injected, scroll still works
        }

        // Register scroll callback
        glfwSetScrollCallback(_window, ScrollCallback);

        // Initialize current mouse position
        double mouseX, mouseY;
        glfwGetCursorPos(_window, &mouseX, &mouseY);
        _currentPosition = Math::Vector2(static_cast<float>(mouseX), static_cast<float>(mouseY));
        _lastPosition = _currentPosition;
    }

    Mouse::~Mouse()
    {
        // Clean up callback
        if (_window)
        {
            glfwSetScrollCallback(_window, nullptr);
        }
    }

    Mouse* Mouse::Get()
    {
        // No input system when the window failed to open
        const auto *input = Application::GetInstance().GetWindow().GetInputSystem();
        return input ? input->GetMouse() : nullptr;
    }

    void Mouse::ScrollCallback(GLFWwindow *window, double xOffset, double yOffset)
    {
        // The window's user pointer is its Window (resize events need it); reach the Mouse through it
        const auto *owner = static_cast<Window *>(glfwGetWindowUserPointer(window));
        auto *input = owner ? owner->GetInputSystem() : nullptr;
        if (auto *mouse = input ? input->GetMouse() : nullptr)
        {
            mouse->AccumulateScroll(static_cast<float>(xOffset), static_cast<float>(yOffset));
        }
    }

    void Mouse::AccumulateScroll(float xOffset, float yOffset)
    {
        // Multiple scroll events can happen between frames - accumulate them until Update
        _pendingScroll.x += xOffset;
        _pendingScroll.y += yOffset;
    }

    void Mouse::InjectPointer(const Math::Vector2 &position, const uint32_t buttons)
    {
        _injected = InjectedPointer{position, buttons};
    }

    void Mouse::Update()
    {
        // This frame's scroll is what arrived since the last Update
        _scrollDelta = _pendingScroll;
        _pendingScroll = Math::Vector2(0.0f, 0.0f);

        // Buttons are sampled here, once per frame, so every reader in the frame sees the same edges
        _previousButtons = _buttons;
        Math::Vector2 position = _currentPosition;
        if (_injected)
        {
            position = _injected->position;
            _buttons = _injected->buttons;
            _injected.reset();
        }
        else if (_window)
        {
            double mouseX, mouseY;
            glfwGetCursorPos(_window, &mouseX, &mouseY);
            position = Math::Vector2(static_cast<float>(mouseX), static_cast<float>(mouseY));

            uint32_t buttons = 0;
            for (int button = 0; button < ButtonCount; ++button)
            {
                if (glfwGetMouseButton(_window, GLFW_MOUSE_BUTTON_1 + button) == GLFW_PRESS)
                {
                    buttons |= ButtonBit(button);
                }
            }
            _buttons = buttons;
        }
        // else: no window and nothing injected; position and buttons are held

        _currentPosition = position;
        _positionDelta = _currentPosition - _lastPosition;
        _lastPosition = _currentPosition;
    }

    bool Mouse::GetButton(const int button) const
    {
        return (_buttons & ButtonBit(button)) != 0;
    }

    bool Mouse::GetButtonDown(const int button) const
    {
        const uint32_t bit = ButtonBit(button);
        return (_buttons & bit) != 0 && (_previousButtons & bit) == 0;
    }

    bool Mouse::GetButtonUp(const int button) const
    {
        const uint32_t bit = ButtonBit(button);
        return (_buttons & bit) == 0 && (_previousButtons & bit) != 0;
    }

    bool Mouse::GetButton(const MouseButton button) const { return GetButton(static_cast<int>(button)); }
    bool Mouse::GetButtonDown(const MouseButton button) const { return GetButtonDown(static_cast<int>(button)); }
    bool Mouse::GetButtonUp(const MouseButton button) const { return GetButtonUp(static_cast<int>(button)); }
}
