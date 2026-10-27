#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "engine/network/NetworkId.hpp"
#include "engine/network/n2net/HashTree.hpp"

namespace N2Engine::Physics
{
    class IPhysicsBackend;
}

namespace N2Engine::Network
{
    class LayoutCache;
    class NetworkWorld;

    /**
     * Builds the lockstep state-hash tree (netcode C3, design 4.5) from the REAL replicated fields of a NetworkWorld.
     *
     * Shape: tick -> one object per registered NetworkObject (object id = net id) -> one leaf per replicated field.
     * A field's leaf is HashTree::LeafHash over the bytes EncodeObjectState produces for it, the very codec the
     * replication wire uses, so a quantised field (Range u16, replicationStep, smallest-three quaternion, 8-bit colour)
     * is hashed in its ENCODED form: two values that encode alike hash alike, and a change smaller than the quantum is
     * not a difference. Field id = the field's index in ObjectLayout::fields, which is the one order both peers derive
     * from the same components. Walking is by ascending net id (NetworkWorld::Objects is an ordered map) and the builder
     * sorts anyway, so no hash-map iteration order reaches the result.
     *
     * Two bookkeeping leaves per object are added, ids that no field index reaches (at most 64 fields):
     *   kLayoutFieldId  the 16-bit layout hash (components and every field's wire format): peers with different layouts
     *                   differ here instead of silently hashing different things alike
     *   kMetaFieldId    owner client id (u32 LE) and a spawned flag (u8)
     * A field that could not be read or encoded this time (empty bytes) still gets a leaf, over zero bytes, so
     * "unreadable" is state too. An object with no replicated fields is just its two bookkeeping leaves.
     *
     * Not covered: anything not marked Replicated (local fields, unreplicated physics state), and objects without a
     * NetworkObject.
     */
    inline constexpr std::uint16_t kLayoutFieldId = 0xFFFF;
    inline constexpr std::uint16_t kMetaFieldId = 0xFFFE;
    /// The physics leaf (netcode C5): IPhysicsBackend::HashSimulatedState as a u64 LE, on object kPhysicsObjectId, which
    /// no net id is (net ids start at 1). It covers the bodies in the simulation, networked or not, and skips parked ones.
    inline constexpr std::uint16_t kPhysicsFieldId = 0xFFFD;
    inline constexpr std::uint32_t kPhysicsObjectId = 0;

    /// nullopt when a HashTree bound is exceeded (more than kMaxTreeObjects objects or kMaxFieldsPerObject leaves)
    [[nodiscard]] std::optional<N2Net::HashTree> BuildStateHashTree(const NetworkWorld &world, LayoutCache &layouts,
                                                                    std::uint32_t tick);

    /// The same tree plus the physics leaf when `physics` is not null and has a HashSimulatedState (what the rollback
    /// runtime exchanges: a rollback peer and a lockstep peer hash the same confirmed state alike, parked bodies or not)
    [[nodiscard]] std::optional<N2Net::HashTree> BuildStateHashTree(const NetworkWorld &world, LayoutCache &layouts,
                                                                    std::uint32_t tick,
                                                                    const Physics::IPhysicsBackend *physics);

    /// A readable name for a leaf the bisection reported: "Component.field" for a field index, "<layout>", "<meta>" or
    /// "<physics>".
    /// Empty when the object or field is not in the world now.
    [[nodiscard]] std::string DescribeHashedField(const NetworkWorld &world, LayoutCache &layouts, std::uint32_t objectId,
                                                  std::uint16_t fieldId);
}
