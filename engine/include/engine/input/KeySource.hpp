#pragma once

#include <set>

#include "engine/input/InputTypes.hpp"

namespace N2Engine::Input
{
    /**
     * Where keyboard keys and mouse buttons are read from. By default the bindings (KeyboardButtonBinding,
     * Vector2CompositeBinding, MouseButtonBinding) poll their window (GLFW). A host that has no keyboard to read, such
     * as the editor's play child (a hidden or absent window, with its input sent by the editor client over the
     * protocol), installs a KeySource, and every binding then asks it instead: the window is never consulted for keys
     * or mouse buttons while one is installed, so a key the window reports can't mix with the injected ones.
     *
     * The mouse position, the scroll and the gamepads aren't part of it: the pointer has Mouse::InjectPointer and
     * Mouse::AccumulateScroll, and gamepads come later.
     *
     * Main thread only. The source is not owned: the installer keeps it alive until it installs another or none.
     */
    class KeySource
    {
    public:
        virtual ~KeySource() = default;

        [[nodiscard]] virtual bool IsKeyDown(Key key) const = 0;
        [[nodiscard]] virtual bool IsMouseButtonDown(MouseButton button) const = 0;

        /// Installs `source` (nullptr: back to polling the window). Returns the one that was installed.
        static const KeySource *Set(const KeySource *source);
        /// The installed source, or nullptr when the bindings poll their window
        [[nodiscard]] static const KeySource *Get();
    };

    /// A KeySource that holds exactly what it was told: a key or button is down from SetKey/SetMouseButton(true) until
    /// the matching false (or ReleaseAll). The editor's SendInput drives one.
    class InjectedKeys final : public KeySource
    {
    public:
        void SetKey(Key key, bool down);
        void SetMouseButton(MouseButton button, bool down);
        /// Every key and button up
        void ReleaseAll();

        [[nodiscard]] bool IsKeyDown(Key key) const override;
        [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const override;
        [[nodiscard]] bool AnyDown() const { return !_keys.empty() || !_buttons.empty(); }

    private:
        std::set<Key> _keys;
        std::set<MouseButton> _buttons;
    };
}
