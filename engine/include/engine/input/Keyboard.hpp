#pragma once

#include <bitset>
#include <optional>
#include <string>
#include <string_view>

#include "engine/input/InputTypes.hpp"

namespace N2Engine::Input
{
    class InputDevice;
}

namespace N2Engine::Input
{
    /**
     * Raw keys and typed text, sampled once per frame by InputSystem::Update before the action maps (as Mouse is).
     *
     * GetKey reads a key as it is this frame; GetKeyDown/GetKeyUp are true for the one frame in which it went down or
     * up. Keys come from the installed KeySource when there is one (the editor's play host, tests), else from the
     * window, with GLFW's sticky keys on so a press shorter than a frame is still seen. Typed text is what GLFW's
     * character callback delivered since the previous frame, as UTF-8 (InjectText adds to it without a window).
     * Action maps are still the way to read game input; this is for text fields, debug consoles and key rebinding.
     */
    class Keyboard
    {
    public:
        static constexpr std::size_t KeyCount = static_cast<std::size_t>(Key::Menu) + 1;
        /// The most typed text kept per frame (bytes); the rest of a burst is dropped
        static constexpr std::size_t MaxTypedBytes = 256;

        explicit Keyboard(InputDevice *window);
        ~Keyboard();

        Keyboard(const Keyboard &) = delete;
        Keyboard &operator=(const Keyboard &) = delete;

        /// The keyboard of the application's input system, or nullptr without a window
        static Keyboard *Get();

        void Update();

        [[nodiscard]] bool GetKey(Key key) const;
        [[nodiscard]] bool GetKeyDown(Key key) const;
        [[nodiscard]] bool GetKeyUp(Key key) const;
        /// This frame's typed characters (UTF-8)
        [[nodiscard]] const std::string &GetTypedText() const { return _typedText; }

        /// Adds characters as if typed (UTF-8); they are this frame's text after the next Update
        void InjectText(std::string_view utf8);
        /// Appends one Unicode code point as UTF-8 (GLFW's character callback)
        void AppendCodepoint(unsigned int codepoint);

        /// A key by its action-map name ("A", "Space", "LeftShift", "Enter"), or nullopt for an unknown name
        static std::optional<Key> KeyFromName(std::string_view name);

    private:
        InputDevice *_window;
        std::bitset<KeyCount> _keys;
        std::bitset<KeyCount> _previousKeys;
        std::string _pendingText;
        std::string _typedText;
    };
}
