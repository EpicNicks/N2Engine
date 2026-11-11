#pragma once

#include <functional>
#include <optional>
#include <string>

#include <renderer/opengl/OpenGLRenderer.hpp>
#include <renderer/common/RendererConfig.hpp>
#if N2ENGINE_RENDERER_VULKAN
#include <renderer/vulkan/VulkanRenderer.hpp>
#endif
#include <renderer/software/SoftwareRenderer.hpp>
#include <math/VectorN.hpp>

#include "engine/config/ApplicationOptions.hpp"
#include "engine/common/Color.hpp"
#include "engine/input/InputDevice.hpp"
#include "engine/platform/PlatformHost.hpp"

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

    class Window
    {
    private:
        /// The platform backend (build targets stage 6; docs/platform.html): desktop-glfw with a window, headless without
        /// one. It owns the window, its events and the input device; it outlives the renderer and the input system.
        std::unique_ptr<Platform::IPlatformHost> _host;
        std::unique_ptr<Renderer::Common::IRenderer> _renderer;
        std::unique_ptr<Input::InputSystem> _inputSystem;
        std::string _title{"N2Engine Application"};
        std::string _initError;
        // InitWindow made a window (the desktop host's surface)
        bool _windowed = false;
        std::function<void(Platform::LifecycleEvent)> _lifecycleHandler;
        /// SetPlatformHost's backend, for the next windowed InitWindow
        std::unique_ptr<Platform::IPlatformHost> _pendingHost;
        /// Hands the host's lifecycle events to _lifecycleHandler
        void ConnectLifecycle();
        bool _rendererFailed = false;
        // Set by SetRenderSize: frames render at this size, whatever the window's
        std::optional<Vector2i> _renderSize;
        // InitWindow took the no-window path (UsesNoWindow): there is a renderer but no window
        bool _windowless = false;
        // The size the windowless renderer started at, which GetWindowDimensions reports in place of a window's
        Vector2i _windowlessSize{0, 0};

        void OnWindowResize(int width, int height);

    public:
        Common::Color clearColor;

    public:
        Window();
        ~Window();

        /// The size a window opens at when there is no monitor to size it from, and the size a windowless
        /// renderer starts at
        static constexpr int FallbackWidth = 1280;
        static constexpr int FallbackHeight = 720;

        /**
         * True when InitWindow creates no window: a headless application on the software renderer. That renderer
         * draws into a CPU buffer, so a hidden window would only give it a GL context to blit to and nothing would
         * ever see it. Without a window GLFW isn't initialised at all, so this works on a machine with no display
         * or GPU (the CI runner): frames are read back with IRenderer::ReadFramebuffer. Also true for a headless
         * application on WebGPU (stage G4 of #155), which has no window path at all: in the web build it renders offscreen
         * the same way (InitWindow refuses WebGPU with a window, and natively, where the backend is a CI harness only).
         */
        [[nodiscard]] static bool UsesNoWindow(const Config::ApplicationOptions &options);

        /// True while GLFW is initialised: from a windowed InitWindow until Shutdown (or a failed init). False with
        /// no window, windowless included. Code that calls GLFW without a window handle (gamepads) checks it, since
        /// GLFW refuses those calls with GLFW_NOT_INITIALIZED otherwise.
        [[nodiscard]] static bool HasGlfw();

        /// @returns false if the window or renderer failed to start; see GetInitError(). With UsesNoWindow(options)
        /// no window is created: the software renderer starts headless at FallbackWidth x FallbackHeight
        /// (SoftwareRenderer::Initialize(nullptr, ...)), and the input system is created as usual. WEBGPU: false, before
        /// any GLFW call, with a window (not headless) or natively; in the web build the WebGPU renderer is made and
        /// initialized headless the same way, which fails until the page hands its device in (stage G5). With
        /// RenderBackend::NONE there is neither a window nor a renderer: it succeeds, IsWindowless() is true,
        /// IsValid() is false and GetRenderer() is null.
        bool InitWindow(const Config::ApplicationOptions &options);
        /// True once InitWindow has succeeded and until Shutdown: a window and a renderer, or (windowless) a
        /// renderer alone
        [[nodiscard]] bool IsValid() const { return (_windowed || _windowless) && _renderer != nullptr; }
        /// True when InitWindow took a no-window path (UsesNoWindow, or RenderBackend::NONE), until Shutdown
        [[nodiscard]] bool IsWindowless() const { return _windowless; }
        [[nodiscard]] const std::string& GetInitError() const { return _initError; }
        /// True when the window opened but the renderer failed (the window is then closed again)
        [[nodiscard]] bool RendererFailed() const { return _rendererFailed; }
        /// True when there is a window and it was asked to close, or when there is nothing to keep running for (no
        /// valid window). A windowless window (a renderer with no window, or RenderBackend::NONE) never closes: its host
        /// ends on its own conditions.
        [[nodiscard]] bool ShouldClose() const;
        /// GLFW's events (with a window), then the input system's update (with or without one)
        void PollEvents();
        void Shutdown();
        void Clear();
        [[nodiscard]] Renderer::Common::IRenderer* GetRenderer() const;
        [[nodiscard]] Input::InputSystem* GetInputSystem() const;
        /// Where input is read from: GLFW's device while a window is open, null without one (input is then injected)
        [[nodiscard]] Input::InputDevice* GetInputDevice() const { return _host ? _host->Input() : nullptr; }
        /// The platform backend InitWindow chose (desktop-glfw or headless), or null before it and after Shutdown
        [[nodiscard]] Platform::IPlatformHost* GetPlatformHost() const { return _host.get(); }
        /// Called with the platform's lifecycle events (pause, resume, surface loss, quit; Platform::LifecycleEvent). The
        /// desktop sends none today; Android and iOS send them all. Set it before or after InitWindow.
        void SetLifecycleHandler(std::function<void(Platform::LifecycleEvent)> handler) { _lifecycleHandler = std::move(handler); }
        /**
         * The platform backend the next windowed InitWindow runs on, in place of desktop-glfw: a player for another
         * platform hands in its own (players/android's, a private target's; build targets stages 8 and 11). Taken by that
         * InitWindow (a windowless InitWindow leaves it for the next one). Null goes back to the build's default.
         */
        void SetPlatformHost(std::unique_ptr<Platform::IPlatformHost> host) { _pendingHost = std::move(host); }

        /// The window's size in screen coordinates; windowless, the size the renderer started at; {0, 0} without either
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
    };
}
