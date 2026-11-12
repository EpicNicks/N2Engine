#include <algorithm>
#include <atomic>
#include <utility>

#include "engine/Window.hpp"
#include "engine/Logger.hpp"
#include "engine/Application.hpp"

#include "engine/input/InputSystem.hpp"
#include "platform/DesktopGlfwHost.hpp"
#include "engine/input/ActionMap.hpp"
#include "engine/input/InputBinding.hpp"

// The WebGPU backend is an engine renderer only in the web build (stage G4 of #155); natively it is the CI test harness (W8)
#if N2ENGINE_RENDERER_WEBGPU && defined(N2ENGINE_PLATFORM_WEB)
#define N2_WINDOW_HAS_WEBGPU 1
#include <renderer/webgpu/WebGpuFactory.hpp>
#else
#define N2_WINDOW_HAS_WEBGPU 0
#endif

using namespace N2Engine;

bool Window::HasGlfw()
{
    return Platform::GlfwInitialized();
}

Window::Window()
    : _renderer(nullptr),
      clearColor(Common::Color::Black)
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
    _windowed = false;
    if (_host)
    {
        _host->DestroySurface();
        _host.reset();
    }
    return false;
}

void Window::ConnectLifecycle()
{
    _host->SetLifecycleHandler([this](const Platform::LifecycleEvent event) {
        if (_lifecycleHandler)
        {
            _lifecycleHandler(event);
        }
    });
}

bool Window::UsesNoWindow(const Config::ApplicationOptions &options)
{
    return options.isHeadless && (options.renderBackend == Config::ApplicationOptions::RenderBackend::SOFTWARE ||
                                  options.renderBackend == Config::ApplicationOptions::RenderBackend::WEBGPU);
}

bool Window::InitWindow(const Config::ApplicationOptions &options)
{
    _initError.clear();
    _rendererFailed = false;
    _renderSize.reset();
    _windowless = false;
    _windowed = false;
    _windowlessSize = {0, 0};

    if (options.renderBackend == Config::ApplicationOptions::RenderBackend::NONE)
    {
        // A dedicated server: no GLFW, no window and no renderer. IsValid() stays false (it needs a renderer), so a host
        // that would draw checks it; Tick with render off and no event polling runs as usual. The input system exists
        // (scripts may ask for a key and get nothing), exactly as it does on a windowless renderer.
        _host = Platform::MakeHeadlessHost();
        ConnectLifecycle();
        _windowless = true;
        _windowlessSize = {0, 0};
        Logger::Log("No renderer and no window (headless server)", Logger::LogLevel::Info);
        _inputSystem = std::make_unique<Input::InputSystem>(*this);
        return true;
    }

    const bool webGpu = options.renderBackend == Config::ApplicationOptions::RenderBackend::WEBGPU;
    if (webGpu && !UsesNoWindow(options))
    {
        // WebGPU has no window path (no surface or swapchain on a GLFW window): it runs headless, in the web player
        return FailInit("The WebGPU renderer runs only without a window (isHeadless), as in the web player");
    }

    if (UsesNoWindow(options))
    {
        // No GLFW at all (glfwInit needs a display on some platforms): the renderer draws offscreen (the software renderer
        // into its CPU buffer, WebGPU into its offscreen target), and frames are read back rather than shown. Window-only
        // calls (SetTitle, SetWindowMode) do nothing.
        std::unique_ptr<Renderer::Common::IRenderer> renderer;
        if (webGpu)
        {
#if N2_WINDOW_HAS_WEBGPU
            renderer = Renderer::WebGpu::CreateWebGpuRenderer();
#else
            return FailInit("The WebGPU renderer can only be chosen in the web build (natively it is the CI test harness of "
                            "the backend, not a renderer an application can run)");
#endif
        }
        else
        {
            renderer = std::make_unique<Renderer::Software::SoftwareRenderer>();
        }
        if (!renderer->Initialize(nullptr, static_cast<uint32_t>(FallbackWidth), static_cast<uint32_t>(FallbackHeight)))
        {
            _rendererFailed = true;
            // WebGPU in the browser build cannot start yet: the page hands its device in at stage G5 (the renderer logs why)
            return FailInit(webGpu ? "Failed to initialize the WebGPU renderer (see the log)" : "Failed to initialize renderer");
        }
        _renderer = std::move(renderer);
        _host = Platform::MakeHeadlessHost();
        _host->CreateSurface({});
        ConnectLifecycle();
        _windowless = true;
        _windowlessSize = {FallbackWidth, FallbackHeight};
        Logger::Log(webGpu ? "Using WebGPU renderer, headless (no window)" : "Using Software renderer, headless (no window)",
                    Logger::LogLevel::Info);
        // Keyboard and mouse bindings read nothing without a window (InputBinding, Mouse), as on a failed one
        _inputSystem = std::make_unique<Input::InputSystem>(*this);
        return true;
    }

    // A window: the desktop platform backend (GLFW). Vulkan has no OpenGL context; the OpenGL renderer's linear lighting
    // needs an sRGB-capable framebuffer (GL_FRAMEBUFFER_SRGB, turned on only for the lit shader); the software renderer
    // presents its own buffer through the context, so its window is left as it was
    _host = Platform::MakeDesktopGlfwHost();
    if (!_host)
    {
        return FailInit("This build has no window backend (no GLFW); run headless");
    }
    Platform::SurfaceOptions surface;
    surface.title = "N2Engine";
    surface.openGlContext = options.renderBackend != Config::ApplicationOptions::RenderBackend::VULKAN;
    surface.srgb = options.renderBackend == Config::ApplicationOptions::RenderBackend::OPENGL;
    surface.visible = !options.isHeadless;
    if (!_host->CreateSurface(surface))
    {
        return FailInit(_host->LastError());
    }
    _windowed = true;
    ConnectLifecycle();
    // Framebuffer resizes (pixels) reach the renderer and the camera
    _host->SetResizeHandler([this](const int width, const int height) { OnWindowResize(width, height); });

    // Create renderer based on configuration
    _renderer = nullptr;

    if (options.renderBackend == Config::ApplicationOptions::RenderBackend::VULKAN)
    {
#if N2ENGINE_RENDERER_VULKAN
        _renderer = std::make_unique<Renderer::Vulkan::VulkanRenderer>();
        Logger::Log("Using Vulkan renderer", Logger::LogLevel::Info);
#else
        return FailInit("This build has no Vulkan renderer (configured with N2ENGINE_RENDERER_VULKAN=OFF)");
#endif
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

    // Initialize the chosen renderer, in pixels: on HiDPI displays the framebuffer is larger than the window
    // The renderer takes the platform's surface and its operations (build targets stage 7): no renderer calls GLFW
    const Platform::NativeSurface native = _host->Surface();
    if (!_renderer || !_renderer->Initialize(_host->RenderSurface(), static_cast<uint32_t>(native.width),
                                             static_cast<uint32_t>(native.height)))
    {
        _rendererFailed = true;
        return FailInit("Failed to initialize renderer");
    }

    _inputSystem = std::make_unique<Input::InputSystem>(*this);
    return true;
}

Vector2i Window::GetWindowDimensions() const
{
    if (!_windowed || !_host)
    {
        return _windowless ? _windowlessSize : Vector2i{0, 0};
    }
    const auto [width, height] = Platform::DesktopWindowSize(*_host);
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
    // The platform's events (a window's input, resize and close); a headless host has none
    if (_host)
    {
        _host->PollEvents();
    }
    if (_inputSystem)
    {
        _inputSystem->Update();
    }
}

void Window::Shutdown()
{
    // Before the window: ~Mouse and ~Keyboard drop their device handlers, and the renderer lets go of the surface
    _inputSystem.reset();
    _renderSize.reset();
    if (_renderer)
    {
        _renderer->Shutdown();
        _renderer.reset();
    }
    if (_host)
    {
        _host->DestroySurface();
        _host.reset();
    }
    _windowed = false;
    _windowless = false;
    _windowlessSize = {0, 0};
}

bool Window::ShouldClose() const
{
    if (_windowed && _host)
    {
        return _host->ShouldClose();
    }
    // A window that failed to open (or was shut down) has nothing to keep running for; a windowless one has
    // nothing that could close (RenderBackend::NONE is windowless without being valid: it never closes either)
    return !IsValid() && !_windowless;
}

void Window::SetTitle(const std::string &title)
{
    _title = title;
    if (_host)
    {
        _host->SetTitle(_title);
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
    if (!_windowed || !_host)
    {
        return;
    }
    switch (windowMode)
    {
    case WindowMode::Windowed: _host->SetWindowMode(Platform::WindowMode::Windowed); break;
    case WindowMode::Fullscreen: _host->SetWindowMode(Platform::WindowMode::Fullscreen); break;
    case WindowMode::BorderlessWindowed: _host->SetWindowMode(Platform::WindowMode::BorderlessWindowed); break;
    }
}
