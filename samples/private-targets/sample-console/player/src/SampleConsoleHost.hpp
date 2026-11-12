#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <engine/platform/PlatformHost.hpp>

namespace SampleConsole
{
    /**
     * The sample private target's platform backend (docs/private-targets.html): a "console" with a fixed 640x360 screen
     * and no window system. Its frames arrive as pixels (Surface::presentPixels) and are summarised on stdout, which is
     * where a real target would hand them to its display API. It brings its own renderer (CreateRenderer).
     */
    class SampleConsoleHost final : public N2Engine::Platform::IPlatformHost
    {
    public:
        static constexpr int Width = 640;
        static constexpr int Height = 360;

        explicit SampleConsoleHost(N2Engine::Platform::HostPaths paths) : _paths(std::move(paths)) {}

        std::string_view Name() const override { return "sample-console"; }
        bool CreateSurface(const N2Engine::Platform::SurfaceOptions &) override { return true; }
        void DestroySurface() override {}
        N2Engine::Platform::NativeSurface Surface() const override;
        Renderer::Common::Surface RenderSurface() const override;
        std::string LastError() const override { return {}; }
        int SurfaceWidth() const override { return Width; }
        int SurfaceHeight() const override { return Height; }
        void SetResizeHandler(std::function<void(int, int)>) override {}
        void SetTitle(const std::string &) override {}
        void SetWindowMode(N2Engine::Platform::WindowMode) override {}
        N2Engine::Platform::WindowMode GetWindowMode() const override { return N2Engine::Platform::WindowMode::Fullscreen; }
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        void SetLifecycleHandler(std::function<void(N2Engine::Platform::LifecycleEvent)>) override {}
        bool OwnsLoop() const override { return false; }
        N2Engine::Input::InputDevice *Input() override { return nullptr; }
        N2Engine::Platform::HostPaths Paths() const override { return _paths; }
        std::unique_ptr<Renderer::Common::IRenderer> CreateRenderer() override;

    private:
        /// A frame: RGBA8, rows bottom to top. Every 30th is summarised (its size and an FNV-1a hash of its pixels)
        void Show(const std::uint8_t *rgba, int width, int height);

        N2Engine::Platform::HostPaths _paths;
        int _frames = 0;
    };
}
