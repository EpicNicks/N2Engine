#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace N2Engine::Network
{
    /// The most NetworkObjects alive at once in one session, scene-placed and spawned together. Every count the wire
    /// carries and every table the world keeps is bounded by it.
    inline constexpr std::uint32_t kMaxNetworkObjects = 4096;

    /// The most entries a prefab table holds (a prefab is named on the wire by its index in the table)
    inline constexpr std::uint16_t kMaxPrefabs = 1024;

    /// A session-wide object id. 0 is never an id. The server hands them out: the scene's NetworkObjects get 1, 2, 3 ...
    /// in the order of their GameObject UUIDs at load, and every object spawned after that the next number up. An id is
    /// never given out twice in a session (the counter only rises), so a message that arrives late can never name a
    /// different object than the one it was about; when the 32-bit counter would run out, the server refuses to spawn.
    struct NetworkId
    {
        std::uint32_t value = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr bool operator==(const NetworkId &other) const noexcept = default;
        [[nodiscard]] constexpr auto operator<=>(const NetworkId &other) const noexcept = default;
    };

    /// Names one connected client for the length of a session. 0 is the server. Like a NetworkId, a client id is never
    /// reused by one server run.
    using ClientId = std::uint32_t;
    inline constexpr ClientId kServerClientId = 0;
}

template <>
struct std::hash<N2Engine::Network::NetworkId>
{
    [[nodiscard]] std::size_t operator()(const N2Engine::Network::NetworkId &id) const noexcept
    {
        return std::hash<std::uint32_t>{}(id.value);
    }
};
