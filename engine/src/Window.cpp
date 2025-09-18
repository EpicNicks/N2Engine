#include "engine/Window.hpp"
#include "engine/Logger.hpp"

using namespace N2Engine;

Window::Window()
    : _window(nullptr), _renderer(nullptr), clearColor(Common::Color::Black())
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

AppRenderer Window::ReadAppRendererFromConfig() const
{
    // TODO
    return AppRenderer::OpenGL;
}