#include "engine/Application.hpp"

#include <renderer/vulkan/VulkanRenderer.hpp>

Application &Application::GetInstance()
{
    static Application instance;
    return instance;
}

void Application::Run()
{
    // vulkan for now
    Renderer::Vulkan::VulkanRenderer renderer;
    for (;;)
    {
        renderer.BeginFrame();
        // renderer.DrawMesh() triangle goes here
        renderer.EndFrame();
        renderer.Present();
    }
}

Application::Application()
{
}