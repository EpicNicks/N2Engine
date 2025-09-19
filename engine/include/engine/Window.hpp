#pragma once

#include <string>

#include <renderer/opengl/OpenGLRenderer.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>
#include <math/VectorN.hpp>

#include "engine/common/Color.hpp"

namespace N2Engine
{
    using Vector2i = Math::VectorN<int, 2>;

    enum class AppRenderer
    {
        Vulkan,
        OpenGL,
    };

    class Window
    {

    private:
        GLFWwindow *_window;
        std::unique_ptr<Renderer::Common::IRenderer> _renderer;
        std::string _title{"N2Engine Application"};

    public:
        Common::Color clearColor;

    public:
        Window();

        void InitWindow();
        bool ShouldClose() const;
        void PollEvents();
        void Shutdown();
        void Clear();
        Renderer::Common::IRenderer *GetRenderer() const;

        Vector2i GetWindowDimensions() const;

        std::string GetTitle() const { return _title; }
        void SetTitle(const std::string &title);

    private:
        AppRenderer ReadAppRendererFromConfig() const;
    };
}
