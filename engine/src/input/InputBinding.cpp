#include "engine/input/InputBinding.hpp"

#include <algorithm>
#include <cmath>

#include "engine/input/InputValue.hpp"
#include "engine/input/KeySource.hpp"
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
    // An installed KeySource (the editor's play child) replaces the window, which may not even exist
    if (const KeySource *source = KeySource::Get())
    {
        return source->IsKeyDown(boundKey);
    }
    // No window (e.g. an InputSystem on a Window that never opened): nothing is pressed, and GLFW is
    // never handed a null window
    if (window == nullptr)
    {
        return false;
    }
    const int glfwKey = KeyToGLFW.at(boundKey);
    const int state = glfwGetKey(window, glfwKey);
    return (state == GLFW_PRESS);
}


float AxisBinding::NormalizeAxis(const GamepadAxis axis, const float raw)
{
    const bool isTrigger = axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
    const float value = isTrigger ? (raw + 1.0f) * 0.5f : raw; // triggers: -1..1 -> 0..1

    const float magnitude = std::abs(value);
    if (magnitude <= Deadzone)
    {
        return 0.0f;
    }
    const float rescaled = std::min(1.0f, (magnitude - Deadzone) / (1.0f - Deadzone));
    return value < 0.0f ? -rescaled : rescaled;
}

InputValue AxisBinding::Shape(const float normalized, const AxisDirection direction, const float pressPoint)
{
    float value = normalized;
    if (direction == AxisDirection::Positive)
    {
        value = std::max(0.0f, normalized);
    }
    else if (direction == AxisDirection::Negative)
    {
        value = std::max(0.0f, -normalized);
    }
    if (pressPoint > 0.0f)
    {
        // A button: the same value type whether or not a gamepad is there, so the action stays a button action
        return std::abs(value) >= pressPoint;
    }
    return value;
}

InputValue AxisBinding::getValue()
{
    // Without GLFW (no window, or windowless) there are no gamepads; GLFW would refuse the call
    if (!Window::HasGlfw())
    {
        return Shape(0.0f, direction, pressPoint);
    }
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        const int glfwAxis = GamepadAxisToGLFW.at(boundAxis);
        return Shape(NormalizeAxis(boundAxis, state.axes[glfwAxis]), direction, pressPoint);
    }
    return Shape(0.0f, direction, pressPoint);
}

InputValue GamepadStickBinding::getValue()
{
    // Without GLFW (no window, or windowless) there are no gamepads; GLFW would refuse the call
    if (!Window::HasGlfw())
    {
        return Vector2(0.0f, 0.0f);
    }
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        const int glfwXAxis = GamepadAxisToGLFW.at(xAxis);
        const int glfwYAxis = GamepadAxisToGLFW.at(yAxis);

        const float x = (invertXAxis ? -1 : 1) * state.axes[glfwXAxis];
        const float y = (invertYAxis ? -1 : 1) * state.axes[glfwYAxis];

        // Apply radial deadzone (better than per-axis deadzone)
        const Vector2 stick(x, y);
        const float magnitude = stick.Length();

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

    // An installed KeySource replaces the window (see KeyboardButtonBinding)
    const KeySource *source = KeySource::Get();

    // No window and no source: no key is pressed (GLFW is never handed a null window)
    if (window == nullptr && source == nullptr)
    {
        return Vector2(x, y);
    }

    const auto isDown = [&](const Key key)
    {
        return source != nullptr ? source->IsKeyDown(key) : glfwGetKey(window, KeyToGLFW.at(key)) == GLFW_PRESS;
    };
    if (isDown(right))
        x += 1.0f;
    if (isDown(left))
        x -= 1.0f;
    if (isDown(up))
        y += 1.0f;
    if (isDown(down))
        y -= 1.0f;

    return Vector2(x, y);
}

InputValue MouseButtonBinding::getValue()
{
    // An installed KeySource replaces the window (see KeyboardButtonBinding)
    if (const KeySource *source = KeySource::Get())
    {
        return source->IsMouseButtonDown(boundButton);
    }
    // No window: no button is pressed (GLFW is never handed a null window)
    if (window == nullptr)
    {
        return false;
    }
    const int glfwButton = MouseButtonToGLFW.at(boundButton);
    const int state = glfwGetMouseButton(window, glfwButton);
    return (state == GLFW_PRESS);
}

InputValue GamepadButtonBinding::getValue()
{
    // Without GLFW (no window, or windowless) there are no gamepads; GLFW would refuse the call
    if (!Window::HasGlfw())
    {
        return false;
    }
    GLFWgamepadstate state;
    if (glfwGetGamepadState(gamepadId, &state))
    {
        const int glfwButton = GamepadButtonToGLFW.at(boundButton);
        return (state.buttons[glfwButton] == GLFW_PRESS);
    }
    return false;
}

Vector2 GamepadButtonCompositeBinding::Combine(const bool upDown, const bool downDown, const bool leftDown,
                                               const bool rightDown)
{
    float x = 0.0f, y = 0.0f;
    if (rightDown)
        x += 1.0f;
    if (leftDown)
        x -= 1.0f;
    if (upDown)
        y += 1.0f;
    if (downDown)
        y -= 1.0f;
    return Vector2(x, y);
}

InputValue GamepadButtonCompositeBinding::getValue()
{
    // Without GLFW (no window, or windowless) there are no gamepads; GLFW would refuse the call
    if (!Window::HasGlfw())
    {
        return Vector2(0.0f, 0.0f);
    }
    GLFWgamepadstate state;
    if (!glfwGetGamepadState(gamepadId, &state))
    {
        return Vector2(0.0f, 0.0f);
    }
    const auto isDown = [&state](const GamepadButton button)
    {
        return state.buttons[GamepadButtonToGLFW.at(button)] == GLFW_PRESS;
    };
    return Combine(isDown(up), isDown(down), isDown(left), isDown(right));
}

InputValue ChordBinding::getValue()
{
    // The main binding is read every frame, so the chord's value type never changes with the modifier
    const InputValue value = binding->getValue();
    if (modifier->getValue().asBool())
    {
        return value;
    }
    if (value.Is<Vector2>())
    {
        return Vector2(0.0f, 0.0f);
    }
    if (value.Is<float>())
    {
        return 0.0f;
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
    nlohmann::json j = {
            {"type", GetType()},
            {"axis", boundAxis},
            {"gamepadId", gamepadId}
    };
    // Written only when set, so maps saved before these fields existed serialize as they did
    if (direction != AxisDirection::Both)
    {
        j["direction"] = direction;
    }
    if (pressPoint > 0.0f)
    {
        j["pressPoint"] = pressPoint;
    }
    return j;
}

nlohmann::json GamepadButtonCompositeBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"up", up},
            {"down", down},
            {"left", left},
            {"right", right},
            {"gamepadId", gamepadId}
    };
}

nlohmann::json ChordBinding::Serialize() const
{
    return {
            {"type", GetType()},
            {"modifier", modifier->Serialize()},
            {"binding", binding->Serialize()}
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