#include "WebInputMap.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace N2Engine::Web
{
    using Input::Key;
    using Input::MouseButton;

    namespace
    {
        const std::unordered_map<std::string, Key> &DomCodes()
        {
            static const std::unordered_map<std::string, Key> codes = [] {
                std::unordered_map<std::string, Key> map = {
                    {"Space", Key::Space},
                    {"Quote", Key::Apostrophe},
                    {"Comma", Key::Comma},
                    {"Minus", Key::Minus},
                    {"Period", Key::Period},
                    {"Slash", Key::Slash},
                    {"Semicolon", Key::Semicolon},
                    {"Equal", Key::Equal},
                    {"BracketLeft", Key::LeftBracket},
                    {"Backslash", Key::Backslash},
                    {"BracketRight", Key::RightBracket},
                    {"Backquote", Key::GraveAccent},
                    {"Escape", Key::Escape},
                    {"Enter", Key::Enter},
                    {"Tab", Key::Tab},
                    {"Backspace", Key::Backspace},
                    {"Insert", Key::Insert},
                    {"Delete", Key::Delete},
                    {"ArrowRight", Key::Right},
                    {"ArrowLeft", Key::Left},
                    {"ArrowDown", Key::Down},
                    {"ArrowUp", Key::Up},
                    {"PageUp", Key::PageUp},
                    {"PageDown", Key::PageDown},
                    {"Home", Key::Home},
                    {"End", Key::End},
                    {"CapsLock", Key::CapsLock},
                    {"ScrollLock", Key::ScrollLock},
                    {"NumLock", Key::NumLock},
                    {"PrintScreen", Key::PrintScreen},
                    {"Pause", Key::Pause},
                    {"NumpadDecimal", Key::KpDecimal},
                    {"NumpadDivide", Key::KpDivide},
                    {"NumpadMultiply", Key::KpMultiply},
                    {"NumpadSubtract", Key::KpSubtract},
                    {"NumpadAdd", Key::KpAdd},
                    {"NumpadEnter", Key::KpEnter},
                    {"NumpadEqual", Key::KpEqual},
                    {"ShiftLeft", Key::LeftShift},
                    {"ControlLeft", Key::LeftControl},
                    {"AltLeft", Key::LeftAlt},
                    {"MetaLeft", Key::LeftSuper},
                    {"ShiftRight", Key::RightShift},
                    {"ControlRight", Key::RightControl},
                    {"AltRight", Key::RightAlt},
                    {"MetaRight", Key::RightSuper},
                    {"ContextMenu", Key::Menu},
                };
                // The runs below are consecutive in the Key enum (InputTypes.hpp), in the order of their names
                for (int i = 0; i < 26; ++i)
                {
                    map.emplace(std::string("Key") + static_cast<char>('A' + i),
                                static_cast<Key>(static_cast<int>(Key::A) + i));
                }
                for (int i = 0; i < 10; ++i)
                {
                    map.emplace(std::string("Digit") + static_cast<char>('0' + i),
                                static_cast<Key>(static_cast<int>(Key::Key0) + i));
                    map.emplace(std::string("Numpad") + static_cast<char>('0' + i),
                                static_cast<Key>(static_cast<int>(Key::Kp0) + i));
                }
                for (int i = 0; i < 12; ++i)
                {
                    map.emplace("F" + std::to_string(i + 1), static_cast<Key>(static_cast<int>(Key::F1) + i));
                }
                return map;
            }();
            return codes;
        }
    }

    Key KeyFromDomCode(const std::string_view code)
    {
        const auto &codes = DomCodes();
        const auto it = codes.find(std::string(code));
        return it == codes.end() ? Key::Unknown : it->second;
    }

    std::optional<MouseButton> MouseButtonFromDom(const int domButton)
    {
        switch (domButton)
        {
        case 0: return MouseButton::Left;
        case 1: return MouseButton::Middle;
        case 2: return MouseButton::Right;
        case 3: return MouseButton::Button4;
        case 4: return MouseButton::Button5;
        default: return std::nullopt;
        }
    }

    bool SuppressesBrowserDefault(const Key key)
    {
        switch (key)
        {
        case Key::Space:
        case Key::Up:
        case Key::Down:
        case Key::Left:
        case Key::Right:
        case Key::PageUp:
        case Key::PageDown:
        case Key::Home:
        case Key::End:
        case Key::Tab:
        case Key::Backspace:
            return true;
        default:
            return false;
        }
    }

    float ClientToRender(const double client, const double origin, const double cssExtent, const int renderExtent)
    {
        if (cssExtent <= 0.0)
        {
            return 0.0f;
        }
        return static_cast<float>((client - origin) / cssExtent * static_cast<double>(renderExtent));
    }

    float WheelToScroll(const double delta, const int domDeltaMode, const bool invert)
    {
        // A notch of a mouse wheel is 100 pixels in Chrome and Edge and 3 lines in Firefox; a page is taken as one notch
        double notches = delta;
        if (domDeltaMode == 0)
        {
            notches = delta / 100.0;
        }
        else if (domDeltaMode == 1)
        {
            notches = delta / 3.0;
        }
        return static_cast<float>(invert ? -notches : notches);
    }

    RenderSize FitRenderSize(const double cssWidth, const double cssHeight, const int maxWidth)
    {
        if (!(cssWidth >= 1.0) || !(cssHeight >= 1.0))
        {
            return {};
        }
        const double scale = cssWidth > maxWidth ? static_cast<double>(maxWidth) / cssWidth : 1.0;
        return {std::max(1, static_cast<int>(std::lround(cssWidth * scale))),
                std::max(1, static_cast<int>(std::lround(cssHeight * scale)))};
    }
}
