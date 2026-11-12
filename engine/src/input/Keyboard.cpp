#include "engine/input/Keyboard.hpp"

#include <nlohmann/json.hpp>

#include "engine/Application.hpp"
#include "engine/Window.hpp"
#include "engine/input/InputDevice.hpp"
#include "engine/input/InputSystem.hpp"
#include "engine/input/KeySource.hpp"

namespace N2Engine::Input
{
    Keyboard::Keyboard(InputDevice *window)
        : _window(window)
    {
        if (_window == nullptr)
        {
            return; // no window: keys come from a KeySource, text from InjectText
        }
        _window->SetTextHandler([this](const unsigned int codepoint) { AppendCodepoint(codepoint); });
        // Sticky keys: a key pressed and released between two Updates still reads as down once
        _window->SetStickyInput(true);
    }

    Keyboard::~Keyboard()
    {
        if (_window != nullptr)
        {
            _window->SetTextHandler({});
        }
    }

    Keyboard *Keyboard::Get()
    {
        const auto *input = Application::GetInstance().GetWindow().GetInputSystem();
        return input ? input->GetKeyboard() : nullptr;
    }

    void Keyboard::AppendCodepoint(const unsigned int codepoint)
    {
        std::string utf8;
        if (codepoint < 0x80)
        {
            utf8 += static_cast<char>(codepoint);
        }
        else if (codepoint < 0x800)
        {
            utf8 += static_cast<char>(0xC0 | (codepoint >> 6));
            utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else if (codepoint < 0x10000)
        {
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
            {
                return; // a lone surrogate is not a character
            }
            utf8 += static_cast<char>(0xE0 | (codepoint >> 12));
            utf8 += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else if (codepoint <= 0x10FFFF)
        {
            utf8 += static_cast<char>(0xF0 | (codepoint >> 18));
            utf8 += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            utf8 += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            utf8 += static_cast<char>(0x80 | (codepoint & 0x3F));
        }
        else
        {
            return;
        }
        InjectText(utf8);
    }

    void Keyboard::InjectText(const std::string_view utf8)
    {
        if (_pendingText.size() + utf8.size() > MaxTypedBytes)
        {
            return; // over the per-frame cap: dropped whole, never split inside a character
        }
        _pendingText.append(utf8);
    }

    void Keyboard::Update()
    {
        _typedText = std::move(_pendingText);
        _pendingText.clear();

        _previousKeys = _keys;
        const KeySource *source = KeySource::Get();
        for (std::size_t i = 1; i < KeyCount; ++i) // 0 is Key::Unknown, never pressed
        {
            const auto key = static_cast<Key>(i);
            bool down = false;
            if (source != nullptr)
            {
                down = source->IsKeyDown(key);
            }
            else if (_window != nullptr)
            {
                down = _window->IsKeyDown(key);
            }
            _keys[i] = down;
        }
    }

    bool Keyboard::GetKey(const Key key) const
    {
        const auto i = static_cast<std::size_t>(key);
        return i < KeyCount && _keys[i];
    }

    bool Keyboard::GetKeyDown(const Key key) const
    {
        const auto i = static_cast<std::size_t>(key);
        return i < KeyCount && _keys[i] && !_previousKeys[i];
    }

    bool Keyboard::GetKeyUp(const Key key) const
    {
        const auto i = static_cast<std::size_t>(key);
        return i < KeyCount && !_keys[i] && _previousKeys[i];
    }

    std::optional<Key> Keyboard::KeyFromName(const std::string_view name)
    {
        // Through the action-map JSON names, accepting a name only if it maps back to itself (an unknown string would
        // otherwise become the enum's first value)
        const nlohmann::json value = std::string(name);
        const Key key = value.get<Key>();
        if (nlohmann::json(key) != value)
        {
            return std::nullopt;
        }
        return key;
    }
}
