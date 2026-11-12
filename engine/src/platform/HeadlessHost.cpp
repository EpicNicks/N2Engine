// The headless platform backend (build targets stage 6; docs/platform.html): no window, no surface, no input device.
// The editor host (Docker included), n2server, the test suites and the replay legs run on it, and so does the web player
// (its page owns the canvas; frames are read back and drawn by the page). Input comes only by injection (KeySource,
// Mouse::InjectPointer, Touchscreen); a lifecycle event is whatever the program hands in (Post, for tests).
#include "engine/platform/PlatformHost.hpp"

// IRenderer complete for CreateRenderer's default (before anything that may include GLFW: glad comes first)
#include <renderer/common/Renderer.hpp>

#include "platform/DesktopGlfwHost.hpp"

namespace N2Engine::Platform
{
    std::string_view LifecycleEventName(const LifecycleEvent event)
    {
        switch (event)
        {
        case LifecycleEvent::Paused: return "Paused";
        case LifecycleEvent::Resumed: return "Resumed";
        case LifecycleEvent::SurfaceLost: return "SurfaceLost";
        case LifecycleEvent::SurfaceRecreated: return "SurfaceRecreated";
        case LifecycleEvent::LowMemory: return "LowMemory";
        case LifecycleEvent::QuitRequested: return "QuitRequested";
        case LifecycleEvent::BackRequested: return "BackRequested";
        }
        return "Unknown";
    }

    namespace
    {
        class HeadlessHost final : public IPlatformHost
        {
        public:
            explicit HeadlessHost(HostPaths paths) : _paths(std::move(paths)) {}

            std::string_view Name() const override { return "headless"; }
            bool CreateSurface(const SurfaceOptions &options) override
            {
                _width = options.width > 0 ? options.width : SurfaceOptions::FallbackWidth;
                _height = options.height > 0 ? options.height : SurfaceOptions::FallbackHeight;
                return true;
            }
            void DestroySurface() override
            {
                _width = 0;
                _height = 0;
            }
            NativeSurface Surface() const override { return {NativeSurface::Kind::None, nullptr, nullptr, _width, _height}; }
            std::string LastError() const override { return {}; }
            int SurfaceWidth() const override { return _width; }
            int SurfaceHeight() const override { return _height; }
            void SetResizeHandler(std::function<void(int, int)> handler) override { _resize = std::move(handler); }
            void SetTitle(const std::string &) override {}
            void SetWindowMode(WindowMode) override {}
            WindowMode GetWindowMode() const override { return WindowMode::Windowed; }
            void PollEvents() override {}
            bool ShouldClose() const override { return false; }
            void SetLifecycleHandler(std::function<void(LifecycleEvent)> handler) override { _lifecycle = std::move(handler); }
            bool OwnsLoop() const override { return false; }
            Input::InputDevice *Input() override { return nullptr; }
            HostPaths Paths() const override { return _paths; }

            void Post(const LifecycleEvent event) const
            {
                if (_lifecycle)
                {
                    _lifecycle(event);
                }
            }

        private:
            HostPaths _paths;
            int _width = 0;
            int _height = 0;
            std::function<void(int, int)> _resize;
            std::function<void(LifecycleEvent)> _lifecycle;
        };
    }

    std::unique_ptr<IPlatformHost> MakeHeadlessHost(HostPaths paths)
    {
        return std::make_unique<HeadlessHost>(std::move(paths));
    }

    std::unique_ptr<Renderer::Common::IRenderer> IPlatformHost::CreateRenderer()
    {
        return nullptr; // the engine's own renderers
    }

    bool PostLifecycleEvent(IPlatformHost &host, const LifecycleEvent event)
    {
        const auto *headless = dynamic_cast<HeadlessHost *>(&host);
        if (headless == nullptr)
        {
            return false;
        }
        headless->Post(event);
        return true;
    }

#ifdef N2ENGINE_NO_GLFW
    // The web and Android have no GLFW: no desktop host (a player hands in its own: Window::SetPlatformHost), and GLFW
    // is never initialised
    std::unique_ptr<IPlatformHost> MakeDesktopGlfwHost(HostPaths)
    {
        return nullptr;
    }
    bool GlfwInitialized()
    {
        return false;
    }
    std::pair<int, int> DesktopWindowSize(IPlatformHost &)
    {
        return {0, 0};
    }
#endif
}
