#include "engine/input/InputBinding.hpp"
#include "engine/input/InputValue.hpp"
#include <engine/input/InputMapping.hpp>
#include <engine/Application.hpp>
#include <engine/Window.hpp>

using namespace N2Engine;
using namespace N2Engine::Input;
using namespace N2Engine::Math;

InputBinding::InputBinding(const Window &win)
    : window(win._window)
{
}

InputValue KeyboardButtonBinding::getValue()
{
    const int glfwKey = KeyToGLFW.at(boundKey);
    const int state = glfwGetKey(window, glfwKey);
    return (state == GLFW_PRESS);
}


InputValue AxisBinding::getValue()
{
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        const int glfwAxis = GamepadAxisToGLFW.at(boundAxis);
        return state.axes[glfwAxis]; // Returns float -1.0 to 1.0
    }
    return 0.0f;
}

InputValue GamepadStickBinding::getValue()
{
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        const int glfwXAxis = GamepadAxisToGLFW.at(xAxis);
        const int glfwYAxis = GamepadAxisToGLFW.at(yAxis);

        const float x = (invertXAxis ? -1 : 1) * state.axes[glfwXAxis];
        const float y = (invertYAxis ? -1 : 1) * state.axes[glfwYAxis];

        // Apply radial deadzone (better than per-axis deadzone)
        const Vector2 stick(x, y);
        const float magnitude = stick.Magnitude();

        if (magnitude < deadzone)
        {
            return Vector2(0.0f, 0.0f);
        }

        // Optional: Normalize the magnitude range after deadzone
        // This prevents a "dead spot" feel
        float normalizedMag = (magnitude - deadzone) / (1.0f - deadzone);
        if (normalizedMag > 1.0f)
            normalizedMag = 1.0f;

        return stick.Normalized() * normalizedMag;
    }

    return Vector2(0.0f, 0.0f);
}

InputValue Vector2CompositeBinding::getValue()
{
    float x = 0.0f, y = 0.0f;

    if (glfwGetKey(window, KeyToGLFW.at(right)) == GLFW_PRESS)
        x += 1.0f;
    if (glfwGetKey(window, KeyToGLFW.at(left)) == GLFW_PRESS)
        x -= 1.0f;
    if (glfwGetKey(window, KeyToGLFW.at(up)) == GLFW_PRESS)
        y += 1.0f;
    if (glfwGetKey(window, KeyToGLFW.at(down)) == GLFW_PRESS)
        y -= 1.0f;

    return Vector2(x, y);
}

InputValue MouseButtonBinding::getValue()
{
    const int glfwButton = MouseButtonToGLFW.at(boundButton);
    const int state = glfwGetMouseButton(window, glfwButton);
    return (state == GLFW_PRESS);
}

InputValue GamepadButtonBinding::getValue()
{
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        const int glfwButton = GamepadButtonToGLFW.at(boundButton);
        return (state.buttons[glfwButton] == GLFW_PRESS);
    }
    return false;
}

nlohmann::json KeyboardButtonBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"key", boundKey}
    };
}

nlohmann::json AxisBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"axis", boundAxis},
            {"gamepadId", gamepadId}
    };
}

nlohmann::json GamepadStickBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"xAxis", xAxis},
            {"yAxis", yAxis},
            {"gamepadId", gamepadId},
            {"deadzone", deadzone},
            {"invertX", invertXAxis},
            {"invertY", invertYAxis}
    };
}

nlohmann::json Vector2CompositeBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"up", up},
            {"down", down},
            {"left", left},
            {"right", right}
    };
}

nlohmann::json MouseButtonBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"button", boundButton}
    };
}

nlohmann::json GamepadButtonBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"button", boundButton},
            {"gamepadId", gamepadId}
    };
}