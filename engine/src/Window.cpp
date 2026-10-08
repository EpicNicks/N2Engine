#include <algorithm>
#include <atomic>
#include <utility>

#include "engine/Window.hpp"
#include "engine/Logger.hpp"
#include "engine/Application.hpp"

#include "engine/input/InputSystem.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"

using namespace N2Engine;

namespace
{
    // Process-wide, as GLFW's own state is: set by a successful glfwInit, cleared by glfwTerminate
    std::atomic<bool> g_glfwInitialized{false};

    void TerminateGlfw()
    {
        glfwTerminate(); // does nothing when GLFW isn't initialised (windowless, or after a failed init)
        g_glfwInitialized = false;
    }
}

bool Window::HasGlfw()
{
    return g_glfwInitialized;
}

Window::Window()
    : _window(nullptr),
      _renderer(nullptr),
      clearColor(Common::Color::Black),
      _windowMode(WindowMode::Windowed)
{
}

Window::~Window() = default;

bool Window::FailInit(const std::string &error)
{
    Logger::Log(error, Logger::LogLevel::Error);
    _initError = error;

    // Leave nothing half-initialized: callers check IsValid() and every accessor handles null
    _inputSystem.reset();
    _renderer.reset();
    _windowless = false;
    _windowlessSize = {0, 0};
    if (_window)
    {
        glfwDestroyWindow(_window);
        _window = nullptr;
    }
    TerminateGlfw();
    return false;
}

bool Window::UsesNoWindow(const Config::ApplicationOptions &options)
{
    return options.isHeadless && options.renderBackend == Config::ApplicationOptions::RenderBackend::SOFTWARE;
}

bool Window::InitWindow(const Config::ApplicationOptions &options)
{
    _initError.clear();
    _rendererFailed = false;
    _renderSize.reset();
    _windowless = false;
    _windowlessSize = {0, 0};

    if (UsesNoWindow(options))
    {
        // No GLFW at all (glfwInit needs a display on some platforms): the renderer draws into its CPU buffer, and
        // frames are read back rather than shown. Window-only calls (SetTitle, SetWindowMode) do nothing.
        auto renderer = std::make_unique<Renderer::Software::SoftwareRenderer>();
        if (!renderer->Initialize(nullptr, static_cast<uint32_t>(FallbackWidth), static_cast<uint32_t>(FallbackHeight)))
        {
            _rendererFailed = true;
            return FailInit("Failed to initialize renderer");
        }
        _renderer = std::move(renderer);
        _windowless = true;
        _windowlessSize = {FallbackWidth, FallbackHeight};
        Logger::Log("Using Software renderer, headless (no window)", Logger::LogLevel::Info);
        // Keyboard and mouse bindings read nothing without a window (InputBinding, Mouse), as on a failed one
        _inputSystem = std::make_unique<Input::InputSystem>(*this);
        return true;
    }

    if (!glfwInit())
    {
        _initError = "Failed to initialize GLFW";
        Logger::Log(_initError, Logger::LogLevel::Error);
        return false;
    }
    g_glfwInitialized = true;

    // Configure GLFW hints based on chosen renderer
    if (options.renderBackend == Config::ApplicationOptions::RenderBackend::VULKAN)
    {
        // Configure for Vulkan - no OpenGL context needed
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    }
    else
    {
        // Configure for OpenGL 3.3 (software renderer also uses glfw to present the framebuffer to the graphics card)
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        // An sRGB-capable framebuffer, for linear lighting (GL_FRAMEBUFFER_SRGB, which the renderer turns on only
        // for the lit shader); it stores what is written as it is while that is off
        glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);

#ifdef __APPLE__
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    }

    if (options.isHeadless)
    {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    // No monitor is attached on some headless machines; fall back to a fixed size
    GLFWmonitor *primaryMonitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *vidMode = primaryMonitor ? glfwGetVideoMode(primaryMonitor) : nullptr;
    const int WIDTH = vidMode ? vidMode->width / 2 : FallbackWidth;
    const int HEIGHT = vidMode ? vidMode->height / 2 : FallbackHeight;

    _window = glfwCreateWindow(WIDTH, HEIGHT, "N2Engine", nullptr, nullptr);
    if (!_window)
    {
        return FailInit("Failed to create GLFW window");
    }

    // Initialize windowed state with current window properties
    windowData.width = WIDTH;
    windowData.height = HEIGHT;
    glfwGetWindowPos(_window, &windowData.posX, &windowData.posY);

    // Framebuffer size is in pixels, which is what the renderer needs (window size is in screen
    // coordinates, which differ on HiDPI displays)
    glfwSetFramebufferSizeCallback(_window, FramebufferSizeCallback);
    glfwSetWindowUserPointer(_window, this);

    // Create renderer based on configuration
    _renderer = nullptr;

    if (options.renderBackend == Config::ApplicationOptions::RenderBackend::VULKAN)
    {
        _renderer = std::make_unique<Renderer::Vulkan::VulkanRenderer>();
        Logger::Log("Using Vulkan renderer", Logger::LogLevel::Info);
    }
    else if (options.renderBackend == Config::ApplicationOptions::RenderBackend::SOFTWARE)
    {
        _renderer = std::make_unique<Renderer::Software::SoftwareRenderer>();
        Logger::Log("Using Software renderer", Logger::LogLevel::Info);
    }
    else
    {
        _renderer = Renderer::OpenGL::CreateOpenGLRenderer();
        Logger::Log("Using OpenGL renderer", Logger::LogLevel::Info);
    }

    // Initialize the chosen renderer
    // The renderer works in pixels; on HiDPI displays the framebuffer is larger than the window size
    int framebufferWidth = WIDTH;
    int framebufferHeight = HEIGHT;
    glfwGetFramebufferSize(_window, &framebufferWidth, &framebufferHeight);

    if (!_renderer || !_renderer->Initialize(_window, static_cast<uint32_t>(framebufferWidth),
                                             static_cast<uint32_t>(framebufferHeight)))
    {
        _rendererFailed = true;
        return FailInit("Failed to initialize renderer");
    }

    _inputSystem = std::make_unique<Input::InputSystem>(*this);
    return true;
}

Vector2i Window::GetWindowDimensions() const
{
    if (!_window)
    {
        return _windowless ? _windowlessSize : Vector2i{0, 0};
    }
    int width, height;
    glfwGetWindowSize(_window, &width, &height);
    return {width, height};
}

void Window::Clear()
{
    if (!_renderer)
    {
        return;
    }
    _renderer->Clear(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
}

Renderer::Common::IRenderer *Window::GetRenderer() const
{
    return _renderer.get();
}

Input::InputSystem *Window::GetInputSystem() const
{
    return _inputSystem.get();
}

void Window::PollEvents()
{
    // GLFW is terminated when the window fails to open, and never initialised without one
    if (_window)
    {
        glfwPollEvents();
    }
    if (_inputSystem)
    {
        _inputSystem->Update();
    }
}

void Window::Shutdown()
{
    // Before the window: ~Mouse unregisters its scroll callback on it
    _inputSystem.reset();
    _renderSize.reset();
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
    _windowless = false;
    _windowlessSize = {0, 0};
    TerminateGlfw();
}

bool Window::ShouldClose() const
{
    if (_window)
    {
        return glfwWindowShouldClose(_window);
    }
    // A window that failed to open (or was shut down) has nothing to keep running for; a windowless one has
    // nothing that could close
    return !IsValid();
}

void Window::SetTitle(const std::string &title)
{
    _title = title;
    if (_window)
    {
        glfwSetWindowTitle(_window, _title.c_str());
    }
}

void Window::FramebufferSizeCallback(GLFWwindow *window, int width, int height)
{
    if (Window *windowInstance = static_cast<Window *>(glfwGetWindowUserPointer(window)))
    {
        windowInstance->OnWindowResize(width, height);
    }
}

void Window::OnWindowResize(int width, int height)
{
    // Frames follow SetRenderSize's size, not the window's
    if (_renderSize)
    {
        return;
    }

    if (_renderer)
    {
        _renderer->OnResize(width, height);
    }

    // Notify the application about the resize so it can update the camera
    Application::GetInstance().OnWindowResize(width, height);
}

bool Window::SetRenderSize(const int width, const int height)
{
    if (!_renderer || width <= 0 || height <= 0)
    {
        return false;
    }
    if (_renderSize && (*_renderSize)[0] == width && (*_renderSize)[1] == height)
    {
        return true;
    }

    if (!_renderer->SetRenderTargetSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height)))
    {
        // The renderer couldn't make the target (OpenGL then renders to the window, without its old target):
        // nothing renders at a fixed size now, and the same size is tried again next call
        _renderSize.reset();
        return false;
    }
    _renderSize = Vector2i{width, height};
    Application::GetInstance().OnWindowResize(width, height);
    return true;
}

void Window::AdoptRenderer(std::unique_ptr<Renderer::Common::IRenderer> renderer)
{
    if (_renderer)
    {
        _renderer->Shutdown();
    }
    _renderer = std::move(renderer);
    _renderSize.reset();
}

Vector2i Window::GetRenderDimensions() const
{
    return _renderSize ? *_renderSize : GetWindowDimensions();
}

void Window::SetWindowMode(WindowMode windowMode)
{
    if (!_window || _windowMode == windowMode)
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