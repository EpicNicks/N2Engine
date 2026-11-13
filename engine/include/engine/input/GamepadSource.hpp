#pragma once

#include <array>
#include <optional>
#include <string>

#include "engine/input/InputDevice.hpp"

namespace N2Engine::Input
{
    /**
     * Where gamepads are read from when the platform's input device can't read them: the gamepad half of KeySource. By
     * default the gamepad bindings (GamepadButton, GamepadAxis, GamepadStick, GamepadButtonComposite) and
     * InputSystem::GetConnectedGamepads ask the window's InputDevice. A host whose gamepads come from somewhere else, such
     * as the web player (the browser's Gamepad API, sampled once a frame, while the engine runs windowless), installs a
     * GamepadSource, and every gamepad read then asks it instead: the window's device is never consulted for gamepads
     * while one is installed.
     *
     * Slots are 0..MaxGamepads-1. Main thread only. The source is not owned: the installer keeps it alive until it installs
     * another or none.
     */
    class GamepadSource
    {
    public:
        virtual ~GamepadSource() = default;

        /// Whether a gamepad with a mapping is at slot id, and if so its state (as InputDevice::GetGamepadState)
        [[nodiscard]] virtual bool GetGamepadState(int id, GamepadState &state) const = 0;
        /// A joystick at slot id (a gamepad or not), and whether it has a gamepad mapping
        [[nodiscard]] virtual bool IsJoystickPresent(int id) const = 0;
        [[nodiscard]] virtual bool IsGamepad(int id) const = 0;
        /// The source's name for a joystick or gamepad slot ("" when there is none)
        [[nodiscard]] virtual std::string GamepadName(int id) const = 0;

        /// Installs `source` (nullptr: back to the window's device). Returns the one that was installed.
        static const GamepadSource *Set(const GamepadSource *source);
        /// The installed source, or nullptr when gamepads are read from the window's device
        [[nodiscard]] static const GamepadSource *Get();
    };

    /// A GamepadSource that holds exactly what it was told: a slot has a pad from Connect until Disconnect (or
    /// DisconnectAll). A pad connected without a state is a joystick with no gamepad mapping (present, not a gamepad, no
    /// state), as GLFW reports a joystick it has no mapping for. The web player drives one.
    class InjectedGamepads final : public GamepadSource
    {
    public:
        /// Puts a pad at slot id (replacing what was there); ignored outside 0..MaxGamepads-1
        void Connect(int id, std::string name, std::optional<GamepadState> state);
        void Disconnect(int id);
        void DisconnectAll();

        [[nodiscard]] bool GetGamepadState(int id, GamepadState &state) const override;
        [[nodiscard]] bool IsJoystickPresent(int id) const override;
        [[nodiscard]] bool IsGamepad(int id) const override;
        [[nodiscard]] std::string GamepadName(int id) const override;

    private:
        struct Pad
        {
            std::string name;
            std::optional<GamepadState> state;
        };
        [[nodiscard]] const Pad *Find(int id) const;

        std::array<std::optional<Pad>, MaxGamepads> _pads;
    };

    /**
     * What the engine reads gamepads from now: the installed GamepadSource, else `device` (a binding's own device, or the
     * application window's when it has none), else nothing. Every gamepad read goes through these, so a source replaces
     * the device everywhere at once.
     */
    [[nodiscard]] bool ReadGamepadState(const InputDevice *device, int id, GamepadState &state);
    [[nodiscard]] bool IsJoystickPresent(const InputDevice *device, int id);
    [[nodiscard]] bool IsGamepad(const InputDevice *device, int id);
    [[nodiscard]] std::string GamepadName(const InputDevice *device, int id);
}
