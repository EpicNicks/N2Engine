#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "engine/network/NetworkId.hpp"
#include "engine/network/n2net/HashTree.hpp"

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

    /// nullopt when a HashTree bound is exceeded (more than kMaxTreeObjects objects or kMaxFieldsPerObject leaves)
    [[nodiscard]] std::optional<N2Net::HashTree> BuildStateHashTree(const NetworkWorld &world, LayoutCache &layouts,
                                                                    std::uint32_t tick);

    /// A readable name for a leaf the bisection reported: "Component.field" for a field index, "<layout>" or "<meta>".
    /// Empty when the object or field is not in the world now.
    [[nodiscard]] std::string DescribeHashedField(const NetworkWorld &world, LayoutCache &layouts, std::uint32_t objectId,
                                                  std::uint16_t fieldId);
}
