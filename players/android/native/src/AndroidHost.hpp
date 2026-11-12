#pragma once

#include <array>
#include <bitset>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <engine/input/InputDevice.hpp>
#include <engine/platform/PlatformHost.hpp>

struct android_app;

namespace N2Engine::Android
{
    /**
     * The Android input device (build targets stage 8; docs/android.html): keys and gamepad buttons from GameActivity's key
     * events, gamepad sticks and triggers from its motion events, all in engine codes. Touches go to the Touchscreen
     * directly (Input::Touchscreen), the first finger also moving the mouse as on the web. Back is Escape.
     */
    class AndroidInputDevice final : public Input::InputDevice
    {
    public:
        bool IsKeyDown(Input::Key key) const override;
        bool IsMouseButtonDown(Input::MouseButton button) const override;
        std::optional<Math::Vector2> CursorPosition() const override;
        bool GetGamepadState(int id, Input::GamepadState &state) const override;
        bool IsJoystickPresent(int id) const override { return id == 0 && _gamepadSeen; }
        bool IsGamepad(int id) const override { return IsJoystickPresent(id); }
        std::string GamepadName(int id) const override { return IsJoystickPresent(id) ? "Android gamepad" : ""; }
        void SetStickyInput(bool) override {}
        void SetScrollHandler(std::function<void(Math::Vector2)>) override {}
        void SetTextHandler(std::function<void(unsigned int)> handler) override { _text = std::move(handler); }

        /// An Android key code went down or up (AKEYCODE_*); false when the engine has no use for it
        bool OnKey(int androidKeyCode, bool down, unsigned int unicode);
        /// A gamepad's axes (AMOTION_EVENT_AXIS_*), from a joystick motion event
        void OnGamepadAxes(float leftX, float leftY, float rightX, float rightY, float leftTrigger, float rightTrigger);
        /// The first finger, as the mouse (position in pixels, held or not)
        void OnPointer(float x, float y, bool down);

    private:
        static constexpr std::size_t KeyCount = static_cast<std::size_t>(Input::Key::Menu) + 1;
        std::bitset<KeyCount> _keys;
        Input::GamepadState _gamepad;
        bool _gamepadSeen = false;
        Math::Vector2 _pointer{0.0f, 0.0f};
        bool _pointerDown = false;
        std::function<void(unsigned int)> _text;
    };

    /**
     * The Android platform backend: GameActivity's window (an ANativeWindow), lifecycle and input. The OS owns the loop
     * (android_main in AndroidMain.cpp calls Application::RunFrame once a frame). The software renderer presents by copying
     * its frame into the window's buffer (Surface::presentPixels), so no EGL context is needed. user:// is the app's files
     * folder, the game's pack is extracted there from the APK's assets on first start.
     */
    class AndroidHost final : public Platform::IPlatformHost
    {
    public:
        AndroidHost(android_app *app, Platform::HostPaths paths);

        std::string_view Name() const override { return "android"; }
        bool CreateSurface(const Platform::SurfaceOptions &options) override;
        void DestroySurface() override;
        Platform::NativeSurface Surface() const override;
        Renderer::Common::Surface RenderSurface() const override;
        std::string LastError() const override { return _error; }
        int SurfaceWidth() const override;
        int SurfaceHeight() const override;
        float ContentScale() const override;
        Platform::SafeAreaInsets Insets() const override;
        void SetResizeHandler(std::function<void(int, int)> handler) override { _resize = std::move(handler); }
        void SetTitle(const std::string &) override {}
        void SetWindowMode(Platform::WindowMode) override {}
        Platform::WindowMode GetWindowMode() const override { return Platform::WindowMode::Fullscreen; }
        void SetTextInputActive(bool active) override;
        void SetKeepScreenOn(bool on) override;
        void PollEvents() override {}
        bool ShouldClose() const override { return _closeRequested; }
        void SetLifecycleHandler(std::function<void(Platform::LifecycleEvent)> handler) override { _lifecycle = std::move(handler); }
        bool OwnsLoop() const override { return true; }
        Input::InputDevice *Input() override { return &_input; }
        Platform::HostPaths Paths() const override { return _paths; }

        // ---- from android_main
        /// The window came (APP_CMD_INIT_WINDOW) or went (APP_CMD_TERM_WINDOW); the size may have changed
        void OnWindowChanged();
        void OnLifecycle(Platform::LifecycleEvent event);
        void RequestClose() { _closeRequested = true; }
        AndroidInputDevice &InputDevice() { return _input; }
        [[nodiscard]] bool HasWindow() const;

    private:
        /// Copies a frame (RGBA8, rows bottom to top) into the window's buffer; dropped while there is no window
        void Present(const std::uint8_t *rgba, int width, int height) const;

        android_app *_app;
        Platform::HostPaths _paths;
        AndroidInputDevice _input;
        std::function<void(int, int)> _resize;
        std::function<void(Platform::LifecycleEvent)> _lifecycle;
        std::string _error;
        bool _closeRequested = false;
        int _lastWidth = 0;
        int _lastHeight = 0;
        mutable std::mutex _windowMutex;
    };
}
