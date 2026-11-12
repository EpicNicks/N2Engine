// The desktop-glfw platform backend (build targets stage 6; docs/platform.html): Windows, Linux and macOS through GLFW.
// The only engine code besides the GLFW input device (input/GlfwInputDevice.cpp) that calls GLFW. Moved from Window.cpp
// as it was: the window's size, hints, window modes and monitors behave exactly as before.
#include "platform/DesktopGlfwHost.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#if N2ENGINE_RENDERER_VULKAN
// glfwCreateWindowSurface is declared only with Vulkan's types
#include <vulkan/vulkan.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "engine/input/InputDevice.hpp"
#include "input/GlfwInputDevice.hpp"

namespace N2Engine::Platform
{
    namespace
    {
        // Process-wide, as GLFW's own state is: set by a successful glfwInit, cleared by glfwTerminate
        std::atomic<bool> g_glfwInitialized{false};

        class DesktopGlfwHost final : public IPlatformHost
        {
        public:
            explicit DesktopGlfwHost(HostPaths paths) : _paths(std::move(paths)) {}
            ~DesktopGlfwHost() override { DestroySurface(); }

            std::string_view Name() const override { return "desktop-glfw"; }

            bool CreateSurface(const SurfaceOptions &options) override
            {
                _error.clear();
                if (!glfwInit())
                {
                    _error = "Failed to initialize GLFW";
                    return false;
                }
                g_glfwInitialized = true;

                if (!options.openGlContext)
                {
                    // Vulkan (and later Metal): no OpenGL context
                    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
                }
                else
                {
                    // OpenGL 3.3 (the software renderer also presents its framebuffer through GLFW's context)
                    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
                    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
                    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
                    if (options.srgb)
                    {
                        glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
                    }
#ifdef __APPLE__
                    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
                }
                if (!options.visible)
                {
                    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
                }

                // No monitor is attached on some headless machines; fall back to a fixed size
                int width = options.width;
                int height = options.height;
                if (width <= 0 || height <= 0)
                {
                    GLFWmonitor *primaryMonitor = glfwGetPrimaryMonitor();
                    const GLFWvidmode *vidMode = primaryMonitor ? glfwGetVideoMode(primaryMonitor) : nullptr;
                    width = vidMode ? vidMode->width / 2 : SurfaceOptions::FallbackWidth;
                    height = vidMode ? vidMode->height / 2 : SurfaceOptions::FallbackHeight;
                }

                _window = glfwCreateWindow(width, height, options.title.c_str(), nullptr, nullptr);
                if (!_window)
                {
                    _error = "Failed to create GLFW window";
                    DestroySurface();
                    return false;
                }
                _windowed = {width, height, 0, 0};
                glfwGetWindowPos(_window, &_windowed.posX, &_windowed.posY);

                // The framebuffer's size is in pixels, which is what a renderer needs (the window's is in screen
                // coordinates, which differ on HiDPI displays)
                glfwSetWindowUserPointer(_window, this);
                glfwSetFramebufferSizeCallback(_window, FramebufferSizeCallback);
                _input = Input::MakeGlfwInputDevice(_window);
                return true;
            }

            void DestroySurface() override
            {
                _input.reset();
                if (_window)
                {
                    glfwDestroyWindow(_window);
                    _window = nullptr;
                }
                glfwTerminate(); // does nothing when GLFW isn't initialised
                g_glfwInitialized = false;
                _mode = WindowMode::Windowed;
            }

            NativeSurface Surface() const override
            {
                if (!_window)
                {
                    return {};
                }
                return {NativeSurface::Kind::GlfwWindow, _window, nullptr, SurfaceWidth(), SurfaceHeight()};
            }

            Renderer::Common::Surface RenderSurface() const override
            {
                Renderer::Common::Surface surface;
                if (!_window)
                {
                    return surface;
                }
                GLFWwindow *window = _window;
                surface.kind = Renderer::Common::Surface::Kind::DesktopWindow;
                surface.handle = window;
                // A window made with an OpenGL context (the OpenGL and software renderers) presents through it
                if (glfwGetWindowAttrib(window, GLFW_CLIENT_API) != GLFW_NO_API)
                {
                    surface.makeContextCurrent = [window] { glfwMakeContextCurrent(window); };
                    surface.getProcAddress = [](const char *name) { return reinterpret_cast<void *>(glfwGetProcAddress(name)); };
                    surface.swapBuffers = [window] { glfwSwapBuffers(window); };
                }
#if N2ENGINE_RENDERER_VULKAN
                surface.vulkanInstanceExtensions = [] {
                    std::vector<const char *> names;
                    if (glfwVulkanSupported() != GLFW_TRUE)
                    {
                        return names; // no Vulkan loader: the renderer refuses the window
                    }
                    uint32_t count = 0;
                    const char **required = glfwGetRequiredInstanceExtensions(&count);
                    if (required != nullptr)
                    {
                        names.assign(required, required + count);
                    }
                    return names;
                };
                surface.createVulkanSurface = [window](void *instance, void *surfaceOut) {
                    return static_cast<int>(glfwCreateWindowSurface(static_cast<VkInstance>(instance), window, nullptr,
                                                                    static_cast<VkSurfaceKHR *>(surfaceOut)));
                };
#endif
                surface.framebufferSize = [window] {
                    int width = 0, height = 0;
                    glfwGetFramebufferSize(window, &width, &height);
                    return std::pair<int, int>{width, height};
                };
                return surface;
            }

            std::string LastError() const override { return _error; }

            int SurfaceWidth() const override { return FramebufferSize().first; }
            int SurfaceHeight() const override { return FramebufferSize().second; }

            /// The window's size in screen coordinates (Window::GetWindowDimensions)
            std::pair<int, int> WindowSize() const
            {
                if (!_window)
                {
                    return {0, 0};
                }
                int width = 0, height = 0;
                glfwGetWindowSize(_window, &width, &height);
                return {width, height};
            }

            void SetResizeHandler(std::function<void(int, int)> handler) override { _resize = std::move(handler); }

            void SetTitle(const std::string &title) override
            {
                if (_window)
                {
                    glfwSetWindowTitle(_window, title.c_str());
                }
            }

            void SetWindowMode(const WindowMode mode) override
            {
                if (!_window || _mode == mode)
                {
                    return;
                }
                if (_mode == WindowMode::Windowed)
                {
                    glfwGetWindowPos(_window, &_windowed.posX, &_windowed.posY);
                    glfwGetWindowSize(_window, &_windowed.width, &_windowed.height);
                }
                switch (mode)
                {
                case WindowMode::Windowed:
                    glfwSetWindowMonitor(_window, nullptr, _windowed.posX, _windowed.posY, _windowed.width, _windowed.height, 0);
                    break;
                case WindowMode::Fullscreen:
                {
                    GLFWmonitor *monitor = CurrentMonitor();
                    const GLFWvidmode *vid = glfwGetVideoMode(monitor);
                    glfwSetWindowMonitor(_window, monitor, 0, 0, vid->width, vid->height, vid->refreshRate);
                    break;
                }
                case WindowMode::BorderlessWindowed:
                {
                    GLFWmonitor *monitor = CurrentMonitor();
                    const GLFWvidmode *vid = glfwGetVideoMode(monitor);
                    int monitorX = 0, monitorY = 0;
                    glfwGetMonitorPos(monitor, &monitorX, &monitorY);
                    glfwSetWindowAttrib(_window, GLFW_DECORATED, GLFW_FALSE);
                    // prevent minimizing when losing focus
                    glfwSetWindowAttrib(_window, GLFW_AUTO_ICONIFY, GLFW_FALSE);
                    glfwSetWindowMonitor(_window, nullptr, monitorX, monitorY, vid->width, vid->height, 0);
                    break;
                }
                }
                // Back to windowed from borderless: decorations return
                if (mode == WindowMode::Windowed && _mode == WindowMode::BorderlessWindowed)
                {
                    glfwSetWindowAttrib(_window, GLFW_DECORATED, GLFW_TRUE);
                    glfwSetWindowAttrib(_window, GLFW_FLOATING, GLFW_FALSE);
                }
                _mode = mode;
            }

            WindowMode GetWindowMode() const override { return _mode; }

            void PollEvents() override
            {
                // GLFW is terminated when the window fails to open, and never initialised without one
                if (_window)
                {
                    glfwPollEvents();
                }
            }

            bool ShouldClose() const override { return _window && glfwWindowShouldClose(_window); }

            void SetLifecycleHandler(std::function<void(LifecycleEvent)> handler) override { _lifecycle = std::move(handler); }
            bool OwnsLoop() const override { return false; }
            Input::InputDevice *Input() override { return _input.get(); }
            HostPaths Paths() const override { return _paths; }

        private:
            std::pair<int, int> FramebufferSize() const
            {
                if (!_window)
                {
                    return {0, 0};
                }
                int width = 0, height = 0;
                glfwGetFramebufferSize(_window, &width, &height);
                return {width, height};
            }

            static void FramebufferSizeCallback(GLFWwindow *window, const int width, const int height)
            {
                auto *host = static_cast<DesktopGlfwHost *>(glfwGetWindowUserPointer(window));
                if (host && host->_resize)
                {
                    host->_resize(width, height);
                }
            }

            /// The monitor the window overlaps most, else the primary one
            GLFWmonitor *CurrentMonitor() const
            {
                int windowX, windowY, windowWidth, windowHeight;
                glfwGetWindowPos(_window, &windowX, &windowY);
                glfwGetWindowSize(_window, &windowWidth, &windowHeight);
                int monitorCount = 0;
                GLFWmonitor **monitors = glfwGetMonitors(&monitorCount);
                GLFWmonitor *best = nullptr;
                int bestOverlap = 0;
                for (int i = 0; i < monitorCount; i++)
                {
                    const GLFWvidmode *vid = glfwGetVideoMode(monitors[i]);
                    int monitorX, monitorY;
                    glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);
                    const int left = std::max(windowX, monitorX);
                    const int top = std::max(windowY, monitorY);
                    const int right = std::min(windowX + windowWidth, monitorX + vid->width);
                    const int bottom = std::min(windowY + windowHeight, monitorY + vid->height);
                    const int overlap = std::max(0, right - left) * std::max(0, bottom - top);
                    if (overlap > bestOverlap)
                    {
                        bestOverlap = overlap;
                        best = monitors[i];
                    }
                }
                return best ? best : glfwGetPrimaryMonitor();
            }

            struct Windowed
            {
                int width = 0, height = 0, posX = 0, posY = 0;
            };

            HostPaths _paths;
            GLFWwindow *_window = nullptr;
            std::unique_ptr<Input::InputDevice> _input;
            std::function<void(int, int)> _resize;
            std::function<void(LifecycleEvent)> _lifecycle;
            WindowMode _mode = WindowMode::Windowed;
            Windowed _windowed;
            std::string _error;
        };
    }

    bool GlfwInitialized()
    {
        return g_glfwInitialized;
    }

    std::pair<int, int> DesktopWindowSize(IPlatformHost &host)
    {
        auto *desktop = dynamic_cast<DesktopGlfwHost *>(&host);
        return desktop ? desktop->WindowSize() : std::pair<int, int>{0, 0};
    }

    std::unique_ptr<IPlatformHost> MakeDesktopGlfwHost(HostPaths paths)
    {
        return std::make_unique<DesktopGlfwHost>(std::move(paths));
    }
}
