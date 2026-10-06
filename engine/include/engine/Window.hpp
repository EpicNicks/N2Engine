#pragma once

#include <optional>
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
        // Set by SetRenderSize: frames render at this size, whatever the window's
        std::optional<Vector2i> _renderSize;

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

        /**
         * Renders every later frame at width x height pixels instead of the window's size
         * (IRenderer::SetRenderTargetSize), for a host that reads frames back rather than showing them: the editor
         * server sets its viewport size here (#69). Once it has succeeded, window resizes no longer reach the
         * renderer or the camera's aspect, which follow this size instead. The same size again does nothing.
         *
         * False, changing nothing, without a renderer or when either dimension isn't positive. False too when the
         * renderer couldn't make a target of that size (SetRenderTargetSize failed): then no render size is set
         * (frames render at the window's size; OpenGL has dropped its old target) and the next call, even with the
         * same size, tries again.
         */
        bool SetRenderSize(int width, int height);
        /**
         * Replaces the renderer, for tests and tools that drive one without InitWindow (a fake, or a headless
         * SoftwareRenderer). The old renderer is shut down and destroyed, and any render size is cleared. No
         * window is created or changed, so IsValid() stays as it was; nullptr just removes the renderer.
         */
        void AdoptRenderer(std::unique_ptr<Renderer::Common::IRenderer> renderer);
        /// The size SetRenderSize set, or GetWindowDimensions() without one: the size the UI pass lays out in, and
        /// the viewport of UI hit testing, world picking and Lua's default Camera:ScreenPointToRay
        [[nodiscard]] Vector2i GetRenderDimensions() const;
        [[nodiscard]] bool HasRenderSize() const { return _renderSize.has_value(); }

        [[nodiscard]] std::string GetTitle() const { return _title; }
        void SetTitle(const std::string &title);

    private:
        bool FailInit(const std::string &error);
        void SaveWindowedState();
        GLFWmonitor* GetCurrentMonitor();
    };
}
