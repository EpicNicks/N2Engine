#pragma once

#include <array>
#include <chrono>

namespace N2Engine::Input
{
    /**
     * Where rumble goes: one gamepad's two motors. The platform's output is used unless another is installed (tests,
     * a host that forwards rumble elsewhere).
     *
     *  - Windows: XInput (xinput1_4.dll, else xinput9_1_0.dll, loaded when first needed, so nothing extra is linked).
     *    Gamepad i is the i-th connected XInput controller, which is GLFW's order for XInput pads but not guaranteed
     *    when other joysticks are plugged in too.
     *  - Web: the Gamepad API's vibrationActuator ("dual-rumble") on navigator.getGamepads()[i], where the browser
     *    has it.
     *  - Elsewhere: none (Set returns false). GLFW has no rumble.
     */
    class RumbleOutput
    {
    public:
        virtual ~RumbleOutput() = default;
        /// Sets both motors, 0..1 each (0, 0 stops); false when the gamepad has no rumble or is not there
        virtual bool Set(int gamepad, float lowFrequency, float highFrequency) = 0;
    };

    /**
     * Gamepad rumble, as Unity's Gamepad.SetMotorSpeeds: the low-frequency (left, heavy) and high-frequency (right,
     * light) motors, 0 to 1. A rumble with a duration stops by itself that much real time later (unscaled: pause and
     * time scale do not stretch it), checked in InputSystem::Update; without one it lasts until Stop. A new Start
     * on a gamepad replaces its rumble. The input system stops every rumble when it shuts down.
     *
     * Main thread only.
     */
    class Rumble
    {
    public:
        using Clock = std::chrono::steady_clock;
        static constexpr int MaxGamepads = 4;

        /// Starts a rumble; seconds <= 0 lasts until Stop. Returns whether the output took it (false: no rumble on
        /// this platform or gamepad, or the index is out of range)
        static bool Start(int gamepad, float lowFrequency, float highFrequency, double seconds,
                          Clock::time_point now = Clock::now());
        static void Stop(int gamepad);
        static void StopAll();
        /// Stops the rumbles whose time is up
        static void Update(Clock::time_point now = Clock::now());
        /// Whether `gamepad` is rumbling now (as far as this module knows)
        [[nodiscard]] static bool IsActive(int gamepad);

        /// Installs `output` (nullptr: the platform's). Returns the one installed before. Not owned.
        static RumbleOutput *SetOutput(RumbleOutput *output);

    private:
        struct State
        {
            bool active = false;
            bool timed = false;
            Clock::time_point until{};
        };
        static std::array<State, MaxGamepads> &States();
        static RumbleOutput &Output();
    };
}
