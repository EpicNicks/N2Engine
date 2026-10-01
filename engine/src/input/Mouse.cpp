#include "engine/input/Mouse.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/Application.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputSystem.hpp"

namespace N2Engine::Input
{
    Mouse::Mouse(GLFWwindow *window)
        : _window(window)
    {
        if (!_window)
        {
            return; // no window (e.g. headless): position and scroll stay zero
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

    void Mouse::Update()
    {
        // This frame's scroll is what arrived since the last Update
        _scrollDelta = _pendingScroll;
        _pendingScroll = Math::Vector2(0.0f, 0.0f);

        if (!_window)
        {
            return;
        }

        double mouseX, mouseY;
        glfwGetCursorPos(_window, &mouseX, &mouseY);
        _currentPosition = Math::Vector2(static_cast<float>(mouseX), static_cast<float>(mouseY));
        _positionDelta = _currentPosition - _lastPosition;
        _lastPosition = _currentPosition;
    }
}
