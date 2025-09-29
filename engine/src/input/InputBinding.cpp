#include "engine/input/InputBinding.hpp"
#include "engine/input/InputValue.hpp"
#include <engine/input/InputMapping.hpp>
#include <engine/Application.hpp>
#include <engine/Window.hpp>

using namespace N2Engine;
using namespace N2Engine::Input;
using namespace N2Engine::Math;

InputBinding::InputBinding(Window &win)
    : window(win._window)
{
}

InputValue ButtonBinding::getValue()
{
    int glfwKey = KeyToGLFW.at(boundKey);
    int state = glfwGetKey(window, glfwKey);
    return (state == GLFW_PRESS);
}

InputValue AxisBinding::getValue()
{
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        int glfwAxis = GamepadAxisToGLFW.at(boundAxis);
        return state.axes[glfwAxis]; // Returns float -1.0 to 1.0
    }
    return 0.0f;
}

InputValue GamepadStickBinding::getValue()
{
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        int glfwXAxis = GamepadAxisToGLFW.at(xAxis);
        int glfwYAxis = GamepadAxisToGLFW.at(yAxis);

        float x = state.axes[glfwXAxis];
        float y = state.axes[glfwYAxis];

        // Apply radial deadzone (better than per-axis deadzone)
        Vector2 stick(x, y);
        float magnitude = stick.Magnitude();

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
    int glfwButton = MouseButtonToGLFW.at(boundButton);
    int state = glfwGetMouseButton(window, glfwButton);
    return (state == GLFW_PRESS);
}

InputValue GamepadButtonBinding::getValue()
{
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        int glfwButton = GamepadButtonToGLFW.at(boundButton);
        return (state.buttons[glfwButton] == GLFW_PRESS);
    }
    return false;
}