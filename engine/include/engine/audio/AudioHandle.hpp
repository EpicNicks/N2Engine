#pragma once
#include <cstdint>
#include <functional>

namespace N2Engine::Audio
{
    struct AudioHandle
    {
        std::uint32_t id = 0;
        [[nodiscard]] bool IsValid() const { return id != 0; }
        bool operator==(const AudioHandle &) const = default;
    };
}

template <>
struct std::hash<N2Engine::Audio::AudioHandle>
{
    size_t operator()(const N2Engine::Audio::AudioHandle &h) const noexcept
    {
        return std::hash<std::uint32_t>{}(h.id);
    }
};
