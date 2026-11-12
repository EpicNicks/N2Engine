#include "engine/input/Mouse.hpp"

#include "engine/Application.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputDevice.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/InputTypes.hpp"

namespace N2Engine::Input
{
    Mouse::Mouse(InputDevice *window)
        : _window(window)
    {
        if (!_window)
        {
            return; // no window (e.g. headless): position and buttons stay as injected, scroll still works
        }

        // Scroll arrives as the device delivers it, between frames
        _window->SetScrollHandler([this](const Math::Vector2 delta) { AccumulateScroll(delta.x, delta.y); });

        // Sticky buttons (and keys): a press released again before Update samples still reads as pressed once, so a
        // click shorter than a frame becomes Down in one Update and Up in the next instead of being lost. Update reads
        // the buttons first in the frame (before action maps poll their mouse bindings).
        _window->SetStickyInput(true);

        // Initialize current mouse position
        if (const auto position = _window->CursorPosition())
        {
            _currentPosition = *position;
        }
        _lastPosition = _currentPosition;
    }

    Mouse::~Mouse()
    {
        if (_window)
        {
            _window->SetScrollHandler({});
        }
    }

    Mouse* Mouse::Get()
    {
        // No input system when the window failed to open
        const auto *input = Application::GetInstance().GetWindow().GetInputSystem();
        return input ? input->GetMouse() : nullptr;
    }

    void Mouse::AccumulateScroll(float xOffset, float yOffset)
    {
        // Multiple scroll events can happen between frames - accumulate them until Update
        _pendingScroll.x += xOffset;
        _pendingScroll.y += yOffset;
    }

    void Mouse::InjectPointer(const Math::Vector2 &position, const uint32_t buttons)
    {
        _injected = InjectedPointer{position, buttons};
    }

    void Mouse::Update()
    {
        // This frame's scroll is what arrived since the last Update
        _scrollDelta = _pendingScroll;
        _pendingScroll = Math::Vector2(0.0f, 0.0f);

        // Buttons are sampled here, once per frame, so every reader in the frame sees the same edges
        _previousButtons = _buttons;
        Math::Vector2 position = _currentPosition;
        if (_injected)
        {
            position = _injected->position;
            _buttons = _injected->buttons;
            _injected.reset();
        }
        else if (_window)
        {
            if (const auto cursor = _window->CursorPosition())
            {
                position = *cursor;
            }

            uint32_t buttons = 0;
            for (int button = 0; button < ButtonCount; ++button)
            {
                if (_window->IsMouseButtonDown(static_cast<MouseButton>(button)))
                {
                    buttons |= ButtonBit(button);
                }
            }
            _buttons = buttons;
        }
        // else: no window and nothing injected; position and buttons are held

        _currentPosition = position;
        _positionDelta = _currentPosition - _lastPosition;
        _lastPosition = _currentPosition;
    }

    bool Mouse::GetButton(const int button) const
    {
        return (_buttons & ButtonBit(button)) != 0;
    }

    bool Mouse::GetButtonDown(const int button) const
    {
        const uint32_t bit = ButtonBit(button);
        return (_buttons & bit) != 0 && (_previousButtons & bit) == 0;
    }

    bool Mouse::GetButtonUp(const int button) const
    {
        const uint32_t bit = ButtonBit(button);
        return (_buttons & bit) == 0 && (_previousButtons & bit) != 0;
    }

    bool Mouse::GetButton(const MouseButton button) const { return GetButton(static_cast<int>(button)); }
    bool Mouse::GetButtonDown(const MouseButton button) const { return GetButtonDown(static_cast<int>(button)); }
    bool Mouse::GetButtonUp(const MouseButton button) const { return GetButtonUp(static_cast<int>(button)); }
}
