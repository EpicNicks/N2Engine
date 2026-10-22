#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/network/NetworkId.hpp"
#include "engine/network/ReplicationCodec.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine
{
    class Component;
    class GameObject;
    class ReferenceResolver;
}

namespace N2Engine::Network
{
    class NetworkWorld;

    /// One component of an object that has replicated fields
    struct ReplicatedComponent
    {
        /// Its position in GameObject::GetAllComponents()
        std::size_t index = 0;
        /// Where its fields are in ObjectLayout::fields
        std::size_t firstField = 0;
        std::size_t fieldCount = 0;
    };

    /**
     * The replicated fields of one object, in the one order both ends agree on: the components that have any, in the
     * order of the object's components, and in each the fields DescribeFields lists with `replicated` set (at most
     * kMaxReplicatedFieldsPerObject in all). The field mask of a state entry has one bit per entry of `fields`.
     * `hash` (16 bits of a hash over the component positions and type names and every field's wire format) travels with
     * each entry, so a reader whose layout is not the writer's skips the entry instead of misreading it.
     */
    struct ObjectLayout
    {
        std::vector<ReplicatedComponent> components;
        std::vector<FieldInfo> fields;
        /// For each entry of `fields`, the index in `components` it belongs to
        std::vector<std::size_t> fieldComponent;
        std::uint16_t hash = 0;
        /// The object's components when the layout was built; the layout is stale when they are not these
        std::vector<const Component *> snapshot;
        /// False for an object with no replicated fields, or whose fields could take more than kMaxReplicatedEntryBytes
        bool usable = false;

        [[nodiscard]] std::size_t MaskBytes() const noexcept { return (fields.size() + 7) / 8; }
    };

    /// Builds the layout of `object` from its components' DescribeFields
    [[nodiscard]] ObjectLayout BuildObjectLayout(GameObject &object);

    /// The layouts of the objects of one world, by net id, rebuilt when an object's components change
    class LayoutCache
    {
    public:
        /// The layout of the object `id` names (a net id is never reused, so a cached layout is the object's own)
        [[nodiscard]] const ObjectLayout &Get(NetworkId id, GameObject &object);
        void Forget(NetworkId id) { _layouts.erase(id.value); }
        void Clear() { _layouts.clear(); }
        [[nodiscard]] std::size_t Size() const noexcept { return _layouts.size(); }

    private:
        std::unordered_map<std::uint32_t, ObjectLayout> _layouts;
    };

    /// How GameObject references cross the wire in one world: GameObject <-> the net id of its NetworkObject
    class WorldReferences final : public IReplicationReferences
    {
    public:
        explicit WorldReferences(const NetworkWorld &world) noexcept : _world(world) {}

        [[nodiscard]] std::uint32_t NetIdOf(const std::string &uuid) const override;
        [[nodiscard]] std::optional<std::string> UuidOf(std::uint32_t netId) const override;

        /// Registers the objects UuidOf handed out, so a reference applied to a component resolves to them
        void RegisterResolved(ReferenceResolver &resolver) const;

    private:
        const NetworkWorld &_world;
        mutable bool _indexed = false;
        mutable std::unordered_map<std::string, std::uint32_t> _byUuid;
        mutable std::vector<std::pair<std::string, GameObject *>> _resolved;
    };

    /// The current wire bytes of every replicated field of one object (empty: the field could not be read or encoded
    /// this time, so it is never sent)
    struct ObjectState
    {
        std::uint16_t layoutHash = 0;
        std::vector<std::vector<std::uint8_t>> fields;
    };

    /// Reads the replicated fields of `object` (Component::SerializeReplicated) and encodes each. Never throws.
    [[nodiscard]] ObjectState EncodeObjectState(const ObjectLayout &layout, GameObject &object,
                                                const IReplicationReferences *references);

    enum class EntryResult : std::uint8_t
    {
        /// The values were applied to the object's components
        Applied,
        /// Well-formed, but not applicable now (a reference to an object this side does not have yet): nothing applied
        Skipped,
        /// The payload is not a valid entry of the layout: nothing applied
        Malformed,
    };

    /// Decodes one entry's payload (the field mask and the fields it names) and applies the values to the object's
    /// components with Component::ApplyReplicated. Nothing is applied unless the whole payload decodes. A component that
    /// refuses a value is counted in `refused` and the rest still applies.
    [[nodiscard]] EntryResult ApplyEntry(const ObjectLayout &layout, GameObject &object, N2Net::ByteReader &payload,
                                         const WorldReferences &references, std::size_t &refused);
}
