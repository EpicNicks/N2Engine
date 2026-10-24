#pragma once
#include "engine/Layers.hpp"
#include "engine/network/NetworkServer.hpp"

namespace N2Engine::Physics
{
    // Server-only: requester must be a joined client. RTT is obtained from its authenticated connection.
    inline std::optional<Network::LagCompensatedHit> RaycastLagCompensated(
        Network::NetworkServer &server, Network::ClientId requester, std::uint64_t renderTimeMicros,
        const Math::Vector3 &origin, const Math::Vector3 &direction, float maxDistance,
        std::uint32_t layers = Layers::DefaultRaycastMask, Network::NetworkId ignore = {})
    {
        return server.CastLagCompensated(requester, renderTimeMicros, origin, direction, maxDistance, 0, layers,
                                         ignore);
    }
    inline std::optional<Network::LagCompensatedHit> SphereCastLagCompensated(
        Network::NetworkServer &server, Network::ClientId requester, std::uint64_t renderTimeMicros,
        const Math::Vector3 &origin, const Math::Vector3 &direction, float maxDistance, float radius,
        std::uint32_t layers = Layers::DefaultRaycastMask, Network::NetworkId ignore = {})
    {
        return server.CastLagCompensated(requester, renderTimeMicros, origin, direction, maxDistance, radius, layers,
                                         ignore);
    }
} // namespace N2Engine::Physics
