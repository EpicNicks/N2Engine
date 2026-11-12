#include "AndroidHost.hpp"

#include <algorithm>
#include <cstring>

#include <android/configuration.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android/window.h>
#include <game-activity/GameActivity.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>

#if N2ENGINE_ANDROID_VULKAN
#define VK_USE_PLATFORM_ANDROID_KHR 1
#include <vulkan/vulkan.h>
#endif

namespace N2Engine::Android
{
    using Input::GamepadAxis;
    using Input::GamepadButton;
    using Input::Key;

    namespace
    {
        /// An Android key code as an engine key (Back is Escape: a game's pause key), or Key::Unknown
        Key KeyOf(const int code)
        {
            if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
            {
                return static_cast<Key>(static_cast<int>(Key::A) + (code - AKEYCODE_A));
            }
            if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
            {
                return static_cast<Key>(static_cast<int>(Key::Key0) + (code - AKEYCODE_0));
            }
            switch (code)
            {
            case AKEYCODE_SPACE: return Key::Space;
            case AKEYCODE_ENTER: return Key::Enter;
            case AKEYCODE_ESCAPE:
            case AKEYCODE_BACK: return Key::Escape;
            case AKEYCODE_TAB: return Key::Tab;
            case AKEYCODE_DEL: return Key::Backspace;
            case AKEYCODE_FORWARD_DEL: return Key::Delete;
            case AKEYCODE_DPAD_UP: return Key::Up;
            case AKEYCODE_DPAD_DOWN: return Key::Down;
            case AKEYCODE_DPAD_LEFT: return Key::Left;
            case AKEYCODE_DPAD_RIGHT: return Key::Right;
            case AKEYCODE_SHIFT_LEFT: return Key::LeftShift;
            case AKEYCODE_SHIFT_RIGHT: return Key::RightShift;
            case AKEYCODE_CTRL_LEFT: return Key::LeftControl;
            case AKEYCODE_CTRL_RIGHT: return Key::RightControl;
            case AKEYCODE_ALT_LEFT: return Key::LeftAlt;
            case AKEYCODE_ALT_RIGHT: return Key::RightAlt;
            case AKEYCODE_COMMA: return Key::Comma;
            case AKEYCODE_PERIOD: return Key::Period;
            case AKEYCODE_MINUS: return Key::Minus;
            case AKEYCODE_EQUALS: return Key::Equal;
            case AKEYCODE_SLASH: return Key::Slash;
            case AKEYCODE_SEMICOLON: return Key::Semicolon;
            case AKEYCODE_APOSTROPHE: return Key::Apostrophe;
            case AKEYCODE_GRAVE: return Key::GraveAccent;
            default: return Key::Unknown;
            }
        }

        /// An Android gamepad key code as an engine gamepad button, or nullopt
        std::optional<GamepadButton> ButtonOf(const int code)
        {
            switch (code)
            {
            case AKEYCODE_BUTTON_A: return GamepadButton::South;
            case AKEYCODE_BUTTON_B: return GamepadButton::East;
            case AKEYCODE_BUTTON_X: return GamepadButton::West;
            case AKEYCODE_BUTTON_Y: return GamepadButton::North;
            case AKEYCODE_BUTTON_L1: return GamepadButton::LeftBumper;
            case AKEYCODE_BUTTON_R1: return GamepadButton::RightBumper;
            case AKEYCODE_BUTTON_SELECT: return GamepadButton::Back;
            case AKEYCODE_BUTTON_START: return GamepadButton::Start;
            case AKEYCODE_BUTTON_MODE: return GamepadButton::Guide;
            case AKEYCODE_BUTTON_THUMBL: return GamepadButton::LeftThumb;
            case AKEYCODE_BUTTON_THUMBR: return GamepadButton::RightThumb;
            case AKEYCODE_DPAD_UP: return GamepadButton::DpadUp;
            case AKEYCODE_DPAD_RIGHT: return GamepadButton::DpadRight;
            case AKEYCODE_DPAD_DOWN: return GamepadButton::DpadDown;
            case AKEYCODE_DPAD_LEFT: return GamepadButton::DpadLeft;
            default: return std::nullopt;
            }
        }
    }

    // ---- input

    bool AndroidInputDevice::IsKeyDown(const Key key) const
    {
        const auto i = static_cast<std::size_t>(key);
        return key != Key::Unknown && i < KeyCount && _keys[i];
    }

    bool AndroidInputDevice::IsMouseButtonDown(const Input::MouseButton button) const
    {
        return button == Input::MouseButton::Left && _pointerDown;
    }

    std::optional<Math::Vector2> AndroidInputDevice::CursorPosition() const
    {
        return _pointer;
    }

    bool AndroidInputDevice::GetGamepadState(const int id, Input::GamepadState &state) const
    {
        if (id != 0 || !_gamepadSeen)
        {
            return false;
        }
        state = _gamepad;
        return true;
    }

    bool AndroidInputDevice::OnKey(const int androidKeyCode, const bool down, const unsigned int unicode)
    {
        bool used = false;
        if (const auto button = ButtonOf(androidKeyCode))
        {
            _gamepadSeen = true;
            _gamepad.buttons[static_cast<std::size_t>(*button)] = down;
            used = true;
        }
        if (const Key key = KeyOf(androidKeyCode); key != Key::Unknown)
        {
            _keys[static_cast<std::size_t>(key)] = down;
            used = true;
        }
        if (down && unicode >= 0x20 && _text)
        {
            _text(unicode);
            used = true;
        }
        return used;
    }

    void AndroidInputDevice::OnGamepadAxes(const float leftX, const float leftY, const float rightX, const float rightY,
                                           const float leftTrigger, const float rightTrigger)
    {
        _gamepadSeen = true;
        _gamepad.axes[static_cast<std::size_t>(GamepadAxis::LeftX)] = leftX;
        _gamepad.axes[static_cast<std::size_t>(GamepadAxis::LeftY)] = leftY;
        _gamepad.axes[static_cast<std::size_t>(GamepadAxis::RightX)] = rightX;
        _gamepad.axes[static_cast<std::size_t>(GamepadAxis::RightY)] = rightY;
        // Android's triggers are 0..1; the engine's rest at -1 and are +1 fully pressed (GLFW's convention)
        _gamepad.axes[static_cast<std::size_t>(GamepadAxis::LeftTrigger)] = leftTrigger * 2.0f - 1.0f;
        _gamepad.axes[static_cast<std::size_t>(GamepadAxis::RightTrigger)] = rightTrigger * 2.0f - 1.0f;
    }

    void AndroidInputDevice::OnPointer(const float x, const float y, const bool down)
    {
        _pointer = Math::Vector2(x, y);
        _pointerDown = down;
    }

    // ---- the host

    AndroidHost::AndroidHost(android_app *app, Platform::HostPaths paths) : _app(app), _paths(std::move(paths))
    {
        // Triggers rest at -1 until a pad says otherwise
        _input.OnGamepadAxes(0, 0, 0, 0, 0, 0);
    }

    bool AndroidHost::HasWindow() const
    {
        std::lock_guard lock(_windowMutex);
        return _app->window != nullptr;
    }

    bool AndroidHost::CreateSurface(const Platform::SurfaceOptions &)
    {
        if (!HasWindow())
        {
            _error = "No Android window yet (the engine starts on APP_CMD_INIT_WINDOW)";
            return false;
        }
        _lastWidth = SurfaceWidth();
        _lastHeight = SurfaceHeight();
        return true;
    }

    void AndroidHost::DestroySurface()
    {
        _lastWidth = 0;
        _lastHeight = 0;
    }

    Platform::NativeSurface AndroidHost::Surface() const
    {
        std::lock_guard lock(_windowMutex);
        if (_app->window == nullptr)
        {
            return {};
        }
        return {Platform::NativeSurface::Kind::AndroidWindow, _app->window, nullptr, ANativeWindow_getWidth(_app->window),
                ANativeWindow_getHeight(_app->window)};
    }

    Renderer::Common::Surface AndroidHost::RenderSurface() const
    {
        Renderer::Common::Surface surface;
        std::lock_guard lock(_windowMutex);
        if (_app->window == nullptr)
        {
            return surface;
        }
        surface.kind = Renderer::Common::Surface::Kind::AndroidWindow;
        surface.handle = _app->window;
        // The window comes and goes (the background): each frame looks it up again rather than keeping this one
        surface.presentPixels = [this](const std::uint8_t *rgba, const int width, const int height) { Present(rgba, width, height); };
        surface.framebufferSize = [this] { return std::pair<int, int>{SurfaceWidth(), SurfaceHeight()}; };
#if N2ENGINE_ANDROID_VULKAN
        // The Vulkan renderer (stage 12) makes its VkSurfaceKHR on the window of the moment: after the app comes back
        // from the background that is a new ANativeWindow (Window hands the renderer this Surface again: OnSurfaceRecreated)
        surface.vulkanInstanceExtensions = [] {
            return std::vector<const char *>{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        };
        surface.createVulkanSurface = [this](void *instance, void *surfaceOut) {
            std::lock_guard windowLock(_windowMutex);
            if (_app->window == nullptr)
            {
                return static_cast<int>(VK_ERROR_SURFACE_LOST_KHR);
            }
            VkAndroidSurfaceCreateInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
            info.window = _app->window;
            return static_cast<int>(vkCreateAndroidSurfaceKHR(static_cast<VkInstance>(instance), &info, nullptr,
                                                              static_cast<VkSurfaceKHR *>(surfaceOut)));
        };
#endif
        return surface;
    }

    void AndroidHost::Present(const std::uint8_t *rgba, const int width, const int height) const
    {
        std::lock_guard lock(_windowMutex);
        ANativeWindow *window = _app->window;
        if (window == nullptr || width <= 0 || height <= 0)
        {
            return; // in the background: the frame is dropped
        }
        // The buffer is the frame's size; the compositor scales it to the window
        ANativeWindow_setBuffersGeometry(window, width, height, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
        ANativeWindow_Buffer buffer;
        if (ANativeWindow_lock(window, &buffer, nullptr) != 0)
        {
            return;
        }
        const int rows = std::min(height, buffer.height);
        const auto rowBytes = static_cast<std::size_t>(std::min(width, buffer.width)) * 4;
        auto *bits = static_cast<std::uint8_t *>(buffer.bits);
        for (int y = 0; y < rows; ++y)
        {
            // The frame's rows are bottom to top; the window's top to bottom
            const std::uint8_t *source = rgba + static_cast<std::size_t>(height - 1 - y) * width * 4;
            std::memcpy(bits + static_cast<std::size_t>(y) * buffer.stride * 4, source, rowBytes);
        }
        ANativeWindow_unlockAndPost(window);
    }

    int AndroidHost::SurfaceWidth() const
    {
        std::lock_guard lock(_windowMutex);
        return _app->window ? ANativeWindow_getWidth(_app->window) : 0;
    }

    int AndroidHost::SurfaceHeight() const
    {
        std::lock_guard lock(_windowMutex);
        return _app->window ? ANativeWindow_getHeight(_app->window) : 0;
    }

    float AndroidHost::ContentScale() const
    {
        const int density = AConfiguration_getDensity(_app->config);
        return density > 0 ? static_cast<float>(density) / 160.0f : 1.0f;
    }

    Platform::SafeAreaInsets AndroidHost::Insets() const
    {
        ARect rect{};
        GameActivity_getWindowInsets(_app->activity, GAMECOMMON_INSETS_TYPE_SYSTEM_BARS, &rect);
        ARect cutout{};
        GameActivity_getWindowInsets(_app->activity, GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT, &cutout);
        return {std::max(rect.left, cutout.left), std::max(rect.top, cutout.top), std::max(rect.right, cutout.right),
                std::max(rect.bottom, cutout.bottom)};
    }

    void AndroidHost::SetTextInputActive(const bool active)
    {
        if (active)
        {
            GameActivity_showSoftInput(_app->activity, GAMEACTIVITY_SHOW_SOFT_INPUT_IMPLICIT);
        }
        else
        {
            GameActivity_hideSoftInput(_app->activity, 0);
        }
    }

    void AndroidHost::SetKeepScreenOn(const bool on)
    {
        if (on)
        {
            GameActivity_setWindowFlags(_app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
        }
        else
        {
            GameActivity_setWindowFlags(_app->activity, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
        }
    }

    void AndroidHost::OnWindowChanged()
    {
        const int width = SurfaceWidth();
        const int height = SurfaceHeight();
        if (width > 0 && height > 0 && (width != _lastWidth || height != _lastHeight))
        {
            _lastWidth = width;
            _lastHeight = height;
            if (_resize)
            {
                _resize(width, height);
            }
        }
    }

    void AndroidHost::OnLifecycle(const Platform::LifecycleEvent event)
    {
        if (event == Platform::LifecycleEvent::QuitRequested)
        {
            _closeRequested = true;
        }
        if (_lifecycle)
        {
            _lifecycle(event);
        }
    }
}
