#include "engine/Application.hpp"
#include "engine/Time.hpp"

#include <renderer/vulkan/VulkanRenderer.hpp>

using namespace N2Engine;

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
        Time::Update();

        renderer.BeginFrame();
        // renderer.DrawMesh() triangle goes here
        renderer.EndFrame();
        renderer.Present();
    }
}

Application::Application()
{
    Time::Init();
}