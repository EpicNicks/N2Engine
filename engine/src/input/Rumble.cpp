#include "engine/input/Rumble.hpp"

#include <algorithm>
#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(N2ENGINE_PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif

namespace N2Engine::Input
{
    namespace
    {
#if defined(_WIN32)
        // XInput's types, declared here so no XInput header or library is needed at build time
        struct XInputVibration
        {
            std::uint16_t leftMotorSpeed;
            std::uint16_t rightMotorSpeed;
        };
        struct XInputGamepad
        {
            std::uint16_t buttons;
            std::uint8_t leftTrigger, rightTrigger;
            std::int16_t thumbLX, thumbLY, thumbRX, thumbRY;
        };
        struct XInputStateData
        {
            std::uint32_t packetNumber;
            XInputGamepad gamepad;
        };
        using SetStateFn = DWORD(WINAPI *)(DWORD, XInputVibration *);
        using GetStateFn = DWORD(WINAPI *)(DWORD, XInputStateData *);
        constexpr DWORD XInputUsers = 4;

        class XInputOutput final : public RumbleOutput
        {
        public:
            bool Set(const int gamepad, const float low, const float high) override
            {
                if (!Load())
                {
                    return false;
                }
                // Gamepad i: the i-th connected controller
                int seen = 0;
                for (DWORD user = 0; user < XInputUsers; ++user)
                {
                    XInputStateData state{};
                    if (_getState(user, &state) != ERROR_SUCCESS)
                    {
                        continue;
                    }
                    if (seen++ == gamepad)
                    {
                        XInputVibration v{Speed(low), Speed(high)}; // left motor: low frequency
                        return _setState(user, &v) == ERROR_SUCCESS;
                    }
                }
                return false;
            }

        private:
            HMODULE _module = nullptr;
            SetStateFn _setState = nullptr;
            GetStateFn _getState = nullptr;
            bool _tried = false;

            static std::uint16_t Speed(const float value)
            {
                return static_cast<std::uint16_t>(std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
            }

            bool Load()
            {
                if (!_tried)
                {
                    _tried = true;
                    for (const char *name : {"xinput1_4.dll", "xinput9_1_0.dll"})
                    {
                        if ((_module = LoadLibraryA(name)) != nullptr)
                        {
                            break;
                        }
                    }
                    if (_module != nullptr)
                    {
                        _setState = reinterpret_cast<SetStateFn>(GetProcAddress(_module, "XInputSetState"));
                        _getState = reinterpret_cast<GetStateFn>(GetProcAddress(_module, "XInputGetState"));
                    }
                }
                return _setState != nullptr && _getState != nullptr;
            }
        };
        using PlatformOutput = XInputOutput;
#elif defined(N2ENGINE_PLATFORM_WEB)
        // The rumble is replayed for its whole length by the module (it stops it with 0, 0), so the browser is asked
        // for a long effect and reset when stopped
        EM_JS(int, n2_web_rumble, (int gamepad, double low, double high), {
            const pads = (typeof navigator !== 'undefined' && navigator.getGamepads) ? navigator.getGamepads() : [];
            const pad = pads ? pads[gamepad] : null;
            const actuator = pad ? pad.vibrationActuator : null;
            if (!actuator) return 0;
            if (low <= 0 && high <= 0) {
                if (actuator.reset) actuator.reset();
                return 1;
            }
            actuator.playEffect('dual-rumble', {duration: 5000, strongMagnitude: low, weakMagnitude: high});
            return 1;
        });

        class WebOutput final : public RumbleOutput
        {
        public:
            bool Set(const int gamepad, const float low, const float high) override
            {
                return n2_web_rumble(gamepad, std::clamp(low, 0.0f, 1.0f), std::clamp(high, 0.0f, 1.0f)) != 0;
            }
        };
        using PlatformOutput = WebOutput;
#else
        class NoOutput final : public RumbleOutput
        {
        public:
            bool Set(int, float, float) override { return false; }
        };
        using PlatformOutput = NoOutput;
#endif

        RumbleOutput *installed = nullptr;
    }

    std::array<Rumble::State, Rumble::MaxGamepads> &Rumble::States()
    {
        static std::array<State, MaxGamepads> states{};
        return states;
    }

    RumbleOutput &Rumble::Output()
    {
        static PlatformOutput platform;
        return installed != nullptr ? *installed : platform;
    }

    RumbleOutput *Rumble::SetOutput(RumbleOutput *output)
    {
        RumbleOutput *previous = installed;
        installed = output;
        return previous;
    }

    bool Rumble::Start(const int gamepad, const float lowFrequency, const float highFrequency, const double seconds,
                       const Clock::time_point now)
    {
        if (gamepad < 0 || gamepad >= MaxGamepads)
        {
            return false;
        }
        State &state = States()[gamepad];
        if (!Output().Set(gamepad, lowFrequency, highFrequency))
        {
            state = {};
            return false;
        }
        state.active = lowFrequency > 0.0f || highFrequency > 0.0f;
        state.timed = state.active && seconds > 0.0;
        state.until = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        return true;
    }

    void Rumble::Stop(const int gamepad)
    {
        if (gamepad < 0 || gamepad >= MaxGamepads)
        {
            return;
        }
        State &state = States()[gamepad];
        if (state.active)
        {
            Output().Set(gamepad, 0.0f, 0.0f);
        }
        state = {};
    }

    void Rumble::StopAll()
    {
        for (int i = 0; i < MaxGamepads; ++i)
        {
            Stop(i);
        }
    }

    void Rumble::Update(const Clock::time_point now)
    {
        for (int i = 0; i < MaxGamepads; ++i)
        {
            const State &state = States()[i];
            if (state.active && state.timed && now >= state.until)
            {
                Stop(i);
            }
        }
    }

    bool Rumble::IsActive(const int gamepad)
    {
        return gamepad >= 0 && gamepad < MaxGamepads && States()[gamepad].active;
    }
}
