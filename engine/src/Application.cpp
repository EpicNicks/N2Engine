#include "engine/Application.hpp"
#include "engine/Time.hpp"
#include "engine/Logger.hpp"

#include <iostream>

#include <renderer/vulkan/VulkanRenderer.hpp>
#include <renderer/opengl/OpenGLRenderer.hpp>
#include <memory>
#include <GLFW/glfw3.h>

using namespace N2Engine;

Application &Application::GetInstance()
{
    static Application instance;
    return instance;
}

void Application::Init()
{
#ifdef N2ENGINE_DEBUG
    Logger::logEvent += [](const std::string &msg, Logger::LogLevel level)
    {
        const char *levelStr = (level == Logger::LogLevel::Info) ? "INFO" : (level == Logger::LogLevel::Warn) ? "WARN"
                                                                                                              : "ERROR";
        std::cout << "[" << levelStr << "] " << msg << std::endl;
    };
#endif
    Time::Init();
}

void Application::Run()
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

    GLFWwindow *window = glfwCreateWindow(WIDTH, HEIGHT, "My Engine", nullptr, nullptr);
    if (!window)
    {
        Logger::Log("Failed to create GLFW window", Logger::LogLevel::Error);
        glfwTerminate();
        return;
    }

    // Create renderer based on configuration
    std::unique_ptr<Renderer::Common::IRenderer> renderer = nullptr;

    if (appRenderer == AppRenderer::Vulkan)
    {
        renderer = std::make_unique<Renderer::Vulkan::VulkanRenderer>();
        Logger::Log("Using Vulkan renderer", Logger::LogLevel::Info);
    }
    else
    {
        renderer = std::make_unique<Renderer::OpenGL::OpenGLRenderer>();
        Logger::Log("Using OpenGL renderer", Logger::LogLevel::Info);
    }

    // Initialize the chosen renderer
    if (!renderer->Initialize(window, WIDTH, HEIGHT))
    {
        Logger::Log("Failed to initialize renderer", Logger::LogLevel::Error);
        glfwDestroyWindow(window);
        glfwTerminate();
        return;
    }

    // Main render loop
    while (!glfwWindowShouldClose(window))
    {
        Time::Update();

        renderer->BeginFrame();

        // Your rendering code goes here
        renderer->Clear(0.1f, 0.1f, 0.1f, 1.0f);

        renderer->EndFrame();
        renderer->Present();

        glfwPollEvents();
    }

    // Cleanup
    renderer.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
}

AppRenderer Application::ReadAppRendererFromConfig() const
{
    // TODO
    return AppRenderer::OpenGL;
}