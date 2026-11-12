#pragma once

#include <utility>

#include "engine/platform/PlatformHost.hpp"

namespace N2Engine::Platform
{
    /// While GLFW is initialised: from a desktop host's successful CreateSurface until DestroySurface (Window::HasGlfw)
    [[nodiscard]] bool GlfwInitialized();
    /// A desktop host's window size in screen coordinates ({0, 0} for another host); the framebuffer can be larger (HiDPI)
    [[nodiscard]] std::pair<int, int> DesktopWindowSize(IPlatformHost &host);
}
