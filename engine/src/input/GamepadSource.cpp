#include "engine/input/GamepadSource.hpp"

#include <utility>

namespace N2Engine::Input
{
    namespace
    {
        const GamepadSource *g_source = nullptr;

        bool InRange(const int id)
        {
            return id >= 0 && id < MaxGamepads;
        }
    }

    const GamepadSource *GamepadSource::Set(const GamepadSource *source)
    {
        return std::exchange(g_source, source);
    }

    const GamepadSource *GamepadSource::Get()
    {
        return g_source;
    }

    void InjectedGamepads::Connect(const int id, std::string name, std::optional<GamepadState> state)
    {
        if (InRange(id))
        {
            _pads[static_cast<std::size_t>(id)] = Pad{std::move(name), std::move(state)};
        }
    }

    void InjectedGamepads::Disconnect(const int id)
    {
        if (InRange(id))
        {
            _pads[static_cast<std::size_t>(id)].reset();
        }
    }

    void InjectedGamepads::DisconnectAll()
    {
        for (auto &pad : _pads)
        {
            pad.reset();
        }
    }

    const InjectedGamepads::Pad *InjectedGamepads::Find(const int id) const
    {
        if (!InRange(id) || !_pads[static_cast<std::size_t>(id)])
        {
            return nullptr;
        }
        return &*_pads[static_cast<std::size_t>(id)];
    }

    bool InjectedGamepads::GetGamepadState(const int id, GamepadState &state) const
    {
        const Pad *pad = Find(id);
        if (pad == nullptr || !pad->state)
        {
            return false;
        }
        state = *pad->state;
        return true;
    }

    bool InjectedGamepads::IsJoystickPresent(const int id) const
    {
        return Find(id) != nullptr;
    }

    bool InjectedGamepads::IsGamepad(const int id) const
    {
        const Pad *pad = Find(id);
        return pad != nullptr && pad->state.has_value();
    }

    std::string InjectedGamepads::GamepadName(const int id) const
    {
        const Pad *pad = Find(id);
        return pad != nullptr ? pad->name : std::string{};
    }

    bool ReadGamepadState(const InputDevice *device, const int id, GamepadState &state)
    {
        if (const GamepadSource *source = GamepadSource::Get())
        {
            return source->GetGamepadState(id, state);
        }
        return device != nullptr && device->GetGamepadState(id, state);
    }

    bool IsJoystickPresent(const InputDevice *device, const int id)
    {
        if (const GamepadSource *source = GamepadSource::Get())
        {
            return source->IsJoystickPresent(id);
        }
        return device != nullptr && device->IsJoystickPresent(id);
    }

    bool IsGamepad(const InputDevice *device, const int id)
    {
        if (const GamepadSource *source = GamepadSource::Get())
        {
            return source->IsGamepad(id);
        }
        return device != nullptr && device->IsGamepad(id);
    }

    std::string GamepadName(const InputDevice *device, const int id)
    {
        if (const GamepadSource *source = GamepadSource::Get())
        {
            return source->GamepadName(id);
        }
        return device != nullptr ? device->GamepadName(id) : std::string{};
    }
}
