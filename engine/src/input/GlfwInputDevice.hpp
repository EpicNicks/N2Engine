#pragma once

#include <memory>

#include "engine/input/InputDevice.hpp"

struct GLFWwindow;

namespace N2Engine::Input
{
    /// The desktop's input device: GLFW's keys, buttons, cursor, scroll, characters and gamepads, translated to engine codes
    /// (GlfwInputMapping.hpp). Window creates it with its GLFW window; nothing else in the engine calls GLFW for input.
    [[nodiscard]] std::unique_ptr<InputDevice> MakeGlfwInputDevice(GLFWwindow *window);
}
