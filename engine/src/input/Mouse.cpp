#include "engine/input/Mouse.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/Application.hpp"
#include "engine/input/InputSystem.hpp"

namespace N2Engine::Input
{
    Mouse::Mouse(GLFWwindow *window)
        : _window(window)
    {
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
        return Application::GetInstance().GetWindow().GetInputSystem()->GetMouse();
    }

    void Mouse::ScrollCallback(GLFWwindow *window, double xOffset, double yOffset)
    {
        if (auto *mouse = static_cast<Mouse*>(glfwGetWindowUserPointer(window)))
        {
            mouse->AccumulateScroll(static_cast<float>(xOffset), static_cast<float>(yOffset));
        }
    }

    void Mouse::AccumulateScroll(float xOffset, float yOffset)
    {
        // Multiple scroll events can happen between frames - accumulate them
        _scrollDelta.x += xOffset;
        _scrollDelta.y += yOffset;
    }

    void Mouse::Update()
    {
        _scrollDelta = Math::Vector2(0.0f, 0.0f);
        double mouseX, mouseY;
        glfwGetCursorPos(_window, &mouseX, &mouseY);
        _currentPosition = Math::Vector2(static_cast<float>(mouseX), static_cast<float>(mouseY));
        _positionDelta = _currentPosition - _lastPosition;
        _lastPosition = _currentPosition;
    }
}
