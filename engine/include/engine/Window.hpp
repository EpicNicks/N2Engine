#pragma once

#include <string>

#include <renderer/opengl/OpenGLRenderer.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>
#include <renderer/software/SoftwareRenderer.hpp>
#include <math/VectorN.hpp>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/common/Color.hpp"

namespace N2Engine
{
    namespace Input
    {
        class InputBinding;
        class InputSystem;
    }

    using Vector2i = Math::VectorN<int, 2>;

    enum class AppRenderer
    {
        Vulkan,
        OpenGL,
    };

    enum class WindowMode
    {
        Windowed,
        Fullscreen,
        BorderlessWindowed
    };

    struct WindowData
    {
        int width, height, posX, posY;
    };

    class Window
    {
        friend class Input::InputBinding;
        friend class Input::InputSystem;

    private:
        GLFWwindow *_window;
        std::unique_ptr<Renderer::Common::IRenderer> _renderer;
        std::unique_ptr<Input::InputSystem> _inputSystem;
        std::string _title{"N2Engine Application"};
        WindowMode _windowMode{WindowMode::Windowed};
        WindowData windowData{};
        std::string _initError;
        bool _rendererFailed = false;

        static void FramebufferSizeCallback(GLFWwindow *window, int width, int height);
        void OnWindowResize(int width, int height);

    public:
        Common::Color clearColor;

    public:
        Window();
        ~Window();

        /// @returns false if the window or renderer failed to start; see GetInitError()
        bool InitWindow(const Config::ApplicationOptions &options);
        /// True once InitWindow has succeeded and until Shutdown
        [[nodiscard]] bool IsValid() const { return _window != nullptr && _renderer != nullptr; }
        [[nodiscard]] const std::string& GetInitError() const { return _initError; }
        /// True when the window opened but the renderer failed (the window is then closed again)
        [[nodiscard]] bool RendererFailed() const { return _rendererFailed; }
        [[nodiscard]] bool ShouldClose() const;
        void PollEvents();
        void Shutdown();
        void Clear();
        [[nodiscard]] Renderer::Common::IRenderer* GetRenderer() const;
        [[nodiscard]] Input::InputSystem* GetInputSystem() const;

        [[nodiscard]] Vector2i GetWindowDimensions() const;
        void SetWindowMode(WindowMode windowMode);

        [[nodiscard]] std::string GetTitle() const { return _title; }
        void SetTitle(const std::string &title);

    private:
        bool FailInit(const std::string &error);
        void SaveWindowedState();
        GLFWmonitor* GetCurrentMonitor();
    };
}
