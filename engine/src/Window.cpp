#include "engine/Window.hpp"
#include "engine/Logger.hpp"
#include "engine/Application.hpp"

#include <algorithm>

using namespace N2Engine;

Window::Window()
    : _window(nullptr),
      _renderer(nullptr),
      clearColor(Common::Color::Black()),
      _windowMode(WindowMode::Windowed)
{
}

void Window::InitWindow()
{
    if (!glfwInit())
    {
        Logger::Log("Failed to initialize GLFW", Logger::LogLevel::Error);
        return;
    }

    // Determine which renderer to use BEFORE creating window
    AppRenderer appRenderer = ReadAppRendererFromConfig();

    // Configure GLFW hints based on chosen renderer
    if (appRenderer == AppRenderer::Vulkan)
    {
        // Configure for Vulkan - no OpenGL context needed
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    else
    {
        // Configure for OpenGL 3.3
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    }

    const GLFWvidmode *vidMode = glfwGetVideoMode(glfwGetPrimaryMonitor());
    const int WIDTH = vidMode->width / 2;
    const int HEIGHT = vidMode->height / 2;

    _window = glfwCreateWindow(WIDTH, HEIGHT, "My Engine", nullptr, nullptr);
    if (!_window)
    {
        Logger::Log("Failed to create GLFW window", Logger::LogLevel::Error);
        glfwTerminate();
        return;
    }

    // Initialize windowed state with current window properties
    windowData.width = WIDTH;
    windowData.height = HEIGHT;
    glfwGetWindowPos(_window, &windowData.posX, &windowData.posY);

    glfwSetWindowSizeCallback(_window, FramebufferSizeCallback);
    glfwSetWindowUserPointer(_window, this);

    // Create renderer based on configuration
    _renderer = nullptr;

    if (appRenderer == AppRenderer::Vulkan)
    {
        _renderer = std::make_unique<Renderer::Vulkan::VulkanRenderer>();
        Logger::Log("Using Vulkan renderer", Logger::LogLevel::Info);
    }
    else
    {
        _renderer = std::make_unique<Renderer::OpenGL::OpenGLRenderer>();
        Logger::Log("Using OpenGL renderer", Logger::LogLevel::Info);
    }

    // Initialize the chosen renderer
    if (!_renderer->Initialize(_window, WIDTH, HEIGHT))
    {
        Logger::Log("Failed to initialize renderer", Logger::LogLevel::Error);
        glfwDestroyWindow(_window);
        glfwTerminate();
        return;
    }
}

Vector2i Window::GetWindowDimensions() const
{
    int width, height;
    glfwGetWindowSize(_window, &width, &height);
    return {width, height};
}

void Window::Clear()
{
    _renderer->Clear(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
}

Renderer::Common::IRenderer *Window::GetRenderer() const
{
    return _renderer.get();
}

void Window::PollEvents()
{
    glfwPollEvents();
}

void Window::Shutdown()
{
    if (_renderer)
    {
        _renderer->Shutdown();
        _renderer.reset();
    }
    if (_window)
    {
        glfwDestroyWindow(_window);
        _window = nullptr;
    }
    glfwTerminate();
}

bool Window::ShouldClose() const
{
    return glfwWindowShouldClose(_window);
}

void Window::SetTitle(const std::string &title)
{
    _title = title;
    if (_window)
    {
        glfwSetWindowTitle(_window, _title.c_str());
    }
}

AppRenderer Window::ReadAppRendererFromConfig() const
{
    // TODO
    return AppRenderer::OpenGL;
}

void Window::FramebufferSizeCallback(GLFWwindow *window, int width, int height)
{
    Window *windowInstance = static_cast<Window *>(glfwGetWindowUserPointer(window));
    if (windowInstance)
    {
        windowInstance->OnWindowResize(width, height);
    }
}

void Window::OnWindowResize(int width, int height)
{
    if (_renderer)
    {
        _renderer->OnResize(width, height);
    }

    // Notify the application about the resize so it can update the camera
    Application::GetInstance().OnWindowResize(width, height);
}

void Window::SetWindowMode(WindowMode windowMode)
{
    if (_windowMode == windowMode)
    {
        return;
    }

    // Save current windowed state if transitioning from windowed mode
    if (_windowMode == WindowMode::Windowed)
    {
        SaveWindowedState();
    }

    switch (windowMode)
    {
    case WindowMode::Windowed:
    {
        // Restore to windowed mode
        glfwSetWindowMonitor(_window, nullptr, windowData.posX, windowData.posY, windowData.width, windowData.height, 0);
        break;
    }
    case WindowMode::Fullscreen:
    {
        // Find which monitor the window is currently on
        GLFWmonitor *monitor = GetCurrentMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);

        glfwSetWindowMonitor(_window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        break;
    }
    case WindowMode::BorderlessWindowed:
    {
        GLFWmonitor *monitor = GetCurrentMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);

        int monitorX, monitorY;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);
        glfwSetWindowAttrib(_window, GLFW_DECORATED, GLFW_FALSE);
        // prevent minimizing when losing focus
        glfwSetWindowAttrib(_window, GLFW_AUTO_ICONIFY, GLFW_FALSE);
        glfwSetWindowMonitor(_window, nullptr, monitorX, monitorY, mode->width, mode->height, 0);

        break;
    }
    }

    // If transitioning back to windowed, restore decorations
    if (windowMode == WindowMode::Windowed && _windowMode == WindowMode::BorderlessWindowed)
    {
        glfwSetWindowAttrib(_window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowAttrib(_window, GLFW_FLOATING, GLFW_FALSE);
    }

    _windowMode = windowMode;
}

void Window::SaveWindowedState()
{
    if (_windowMode == WindowMode::Windowed)
    {
        glfwGetWindowPos(_window, &windowData.posX, &windowData.posY);
        glfwGetWindowSize(_window, &windowData.width, &windowData.height);
    }
}

GLFWmonitor *Window::GetCurrentMonitor()
{
    int windowX, windowY, windowWidth, windowHeight;
    glfwGetWindowPos(_window, &windowX, &windowY);
    glfwGetWindowSize(_window, &windowWidth, &windowHeight);

    int monitorCount;
    GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);

    GLFWmonitor *bestMonitor = nullptr;
    int bestOverlap = 0;

    for (int i = 0; i < monitorCount; i++)
    {
        GLFWmonitor *monitor = monitors[i];
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        int monitorX, monitorY;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);

        // Calculate overlap between window and monitor
        int overlapLeft = std::max(windowX, monitorX);
        int overlapTop = std::max(windowY, monitorY);
        int overlapRight = std::min(windowX + windowWidth, monitorX + mode->width);
        int overlapBottom = std::min(windowY + windowHeight, monitorY + mode->height);

        int overlapArea = std::max(0, overlapRight - overlapLeft) * std::max(0, overlapBottom - overlapTop);

        if (overlapArea > bestOverlap)
        {
            bestOverlap = overlapArea;
            bestMonitor = monitor;
        }
    }

    // Fallback to primary monitor if no overlap found
    return bestMonitor ? bestMonitor : glfwGetPrimaryMonitor();
}