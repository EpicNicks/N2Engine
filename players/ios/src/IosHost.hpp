#pragma once

#include <bitset>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <engine/input/InputDevice.hpp>
#include <engine/platform/PlatformHost.hpp>

namespace N2Engine::Ios
{
    /**
     * The iOS input device (build targets stage 10; docs/apple.html): the first finger as the mouse, as on Android and the
     * web. Touches go to the Touchscreen directly (IosMain.mm); a hardware keyboard and game controllers are follow-ups.
     */
    class IosInputDevice final : public Input::InputDevice
    {
    public:
        bool IsKeyDown(Input::Key) const override { return false; }
        bool IsMouseButtonDown(Input::MouseButton button) const override
        {
            return button == Input::MouseButton::Left && _pointerDown;
        }
        std::optional<Math::Vector2> CursorPosition() const override { return _pointer; }
        bool GetGamepadState(int, Input::GamepadState &) const override { return false; }
        bool IsJoystickPresent(int) const override { return false; }
        bool IsGamepad(int) const override { return false; }
        std::string GamepadName(int) const override { return ""; }
        void SetStickyInput(bool) override {}
        void SetScrollHandler(std::function<void(Math::Vector2)>) override {}
        void SetTextHandler(std::function<void(unsigned int)>) override {}

        void OnPointer(float x, float y, bool down)
        {
            _pointer = Math::Vector2(x, y);
            _pointerDown = down;
        }

    private:
        Math::Vector2 _pointer{0.0f, 0.0f};
        bool _pointerDown = false;
    };

    /**
     * What the host needs from the app's view, supplied by IosMain.mm (Objective-C stays out of this header): its size in
     * pixels, the safe area, the scale, and showing a frame.
     */
    struct ViewBridge
    {
        std::function<std::pair<int, int>()> sizeInPixels;
        std::function<Platform::SafeAreaInsets()> insets;
        std::function<float()> scale;
        /// A frame: RGBA8, rows bottom to top (the software renderer's layout)
        std::function<void(const std::uint8_t *rgba, int width, int height)> show;
        std::function<void(bool on)> keepScreenOn;
        /// The layer the frame is shown on (a CALayer*), for NativeSurface
        void *layer = nullptr;
    };

    /**
     * The iOS platform backend: a UIKit view, its lifecycle and touch. The OS owns the loop (a CADisplayLink calls
     * Application::RunFrame once a frame). The software renderer presents by handing its pixels over (Surface::presentPixels),
     * which become the view's layer contents. user:// is the app's Application Support folder; the game's pack is read in
     * place from the app bundle.
     */
    class IosHost final : public Platform::IPlatformHost
    {
    public:
        IosHost(ViewBridge view, Platform::HostPaths paths) : _view(std::move(view)), _paths(std::move(paths)) {}

        std::string_view Name() const override { return "ios"; }
        bool CreateSurface(const Platform::SurfaceOptions &) override { return true; }
        void DestroySurface() override {}
        Platform::NativeSurface Surface() const override
        {
            const auto [width, height] = _view.sizeInPixels();
            return {Platform::NativeSurface::Kind::MetalLayer, _view.layer, nullptr, width, height};
        }
        Renderer::Common::Surface RenderSurface() const override
        {
            Renderer::Common::Surface surface;
            surface.kind = Renderer::Common::Surface::Kind::MetalLayer;
            surface.handle = _view.layer;
            surface.presentPixels = _view.show;
            surface.framebufferSize = _view.sizeInPixels;
            return surface;
        }
        std::string LastError() const override { return {}; }
        int SurfaceWidth() const override { return _view.sizeInPixels().first; }
        int SurfaceHeight() const override { return _view.sizeInPixels().second; }
        float ContentScale() const override { return _view.scale(); }
        Platform::SafeAreaInsets Insets() const override { return _view.insets(); }
        void SetResizeHandler(std::function<void(int, int)> handler) override { _resize = std::move(handler); }
        void SetTitle(const std::string &) override {}
        void SetWindowMode(Platform::WindowMode) override {}
        Platform::WindowMode GetWindowMode() const override { return Platform::WindowMode::Fullscreen; }
        void SetKeepScreenOn(bool on) override
        {
            if (_view.keepScreenOn) _view.keepScreenOn(on);
        }
        void PollEvents() override {}
        bool ShouldClose() const override { return false; } // iOS apps do not quit themselves
        void SetLifecycleHandler(std::function<void(Platform::LifecycleEvent)> handler) override { _lifecycle = std::move(handler); }
        bool OwnsLoop() const override { return true; }
        Input::InputDevice *Input() override { return &_input; }
        Platform::HostPaths Paths() const override { return _paths; }

        // ---- from IosMain.mm
        IosInputDevice &InputDevice() { return _input; }
        void OnLifecycle(Platform::LifecycleEvent event)
        {
            if (_lifecycle) _lifecycle(event);
        }
        /// The view's size changed (a rotation): the engine hears the new size in pixels
        void OnResized()
        {
            const auto [width, height] = _view.sizeInPixels();
            if ((width != _lastWidth || height != _lastHeight) && _resize)
            {
                _resize(width, height);
            }
            _lastWidth = width;
            _lastHeight = height;
        }

    private:
        ViewBridge _view;
        Platform::HostPaths _paths;
        IosInputDevice _input;
        std::function<void(int, int)> _resize;
        std::function<void(Platform::LifecycleEvent)> _lifecycle;
        int _lastWidth = 0;
        int _lastHeight = 0;
    };
}
