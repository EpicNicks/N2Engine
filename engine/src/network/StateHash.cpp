#include "engine/network/StateHash.hpp"

#include <array>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"

namespace N2Engine::Network
{
    std::optional<N2Net::HashTree> BuildStateHashTree(const NetworkWorld &world, LayoutCache &layouts, const std::uint32_t tick)
    {
        N2Net::HashTreeBuilder builder(tick);
        const WorldReferences references(world);

        for (const auto &entry : world.Objects())
        {
            NetworkObject *const networkObject = entry.second;
            if (networkObject == nullptr)
            {
                continue;
            }
            const std::uint32_t id = entry.first;
            GameObject &gameObject = networkObject->GetGameObject();
            const ObjectLayout &layout = layouts.Get(NetworkId{id}, gameObject);

            if (!builder.AddObject(id))
            {
                return std::nullopt;
            }

            const std::array<std::uint8_t, 2> layoutBytes = {static_cast<std::uint8_t>(layout.hash & 0xFFu),
                                                             static_cast<std::uint8_t>(layout.hash >> 8)};
            const ClientId owner = networkObject->GetOwner();
            const std::array<std::uint8_t, 5> meta = {
                static_cast<std::uint8_t>(owner & 0xFFu), static_cast<std::uint8_t>((owner >> 8) & 0xFFu),
                static_cast<std::uint8_t>((owner >> 16) & 0xFFu), static_cast<std::uint8_t>((owner >> 24) & 0xFFu),
                static_cast<std::uint8_t>(networkObject->IsSpawned() ? 1 : 0)};
            if (!builder.AddField(id, kLayoutFieldId, layoutBytes) || !builder.AddField(id, kMetaFieldId, meta))
            {
                return std::nullopt;
            }

            if (!layout.usable)
            {
                continue;
            }
            const ObjectState state = EncodeObjectState(layout, gameObject, &references);
            for (std::size_t i = 0; i < state.fields.size(); ++i)
            {
                if (!builder.AddField(id, static_cast<std::uint16_t>(i), state.fields[i]))
                {
                    return std::nullopt;
                }
            }
        }
        return builder.Build();
    }

    std::string DescribeHashedField(const NetworkWorld &world, LayoutCache &layouts, const std::uint32_t objectId,
                                    const std::uint16_t fieldId)
    {
        NetworkObject *const networkObject = world.Find(NetworkId{objectId});
        if (networkObject == nullptr)
        {
            return {};
        }
        if (fieldId == kLayoutFieldId)
        {
            return "<layout>";
        }
        if (fieldId == kMetaFieldId)
        {
            return "<meta>";
        }
        GameObject &gameObject = networkObject->GetGameObject();
        const ObjectLayout &layout = layouts.Get(NetworkId{objectId}, gameObject);
        if (fieldId >= layout.fields.size())
        {
            return {};
        }
        std::string name;
        const auto &components = gameObject.GetAllComponents();
        const std::size_t componentSlot = layout.fieldComponent[fieldId];
        if (componentSlot < layout.components.size())
        {
            const std::size_t index = layout.components[componentSlot].index;
            if (index < components.size() && components[index] != nullptr)
            {
                name = components[index]->GetTypeName() + ".";
            }
        }
        return name + layout.fields[fieldId].name;
    }
}
