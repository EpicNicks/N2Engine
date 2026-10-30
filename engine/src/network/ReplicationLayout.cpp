#include "engine/network/ReplicationLayout.hpp"

#include <exception>
#include <format>
#include <span>

#include <math/UUID.hpp>

#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/network/NetworkHash.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

namespace N2Engine::Network
{
    namespace
    {
        std::vector<std::weak_ptr<const bool>> SnapshotOf(const GameObject &object)
        {
            std::vector<std::weak_ptr<const bool>> snapshot;
            snapshot.reserve(object.GetAllComponents().size());
            for (const std::unique_ptr<Component> &component : object.GetAllComponents())
            {
                snapshot.push_back(component ? component->GetLifetimeToken() : std::weak_ptr<const bool>());
            }
            return snapshot;
        }

        /// The same owner (control block): two empty ones are the same too
        bool SameOwner(const std::weak_ptr<const bool> &a, const std::weak_ptr<const bool> &b) noexcept
        {
            return !a.owner_before(b) && !b.owner_before(a);
        }

        std::uint16_t Fold(const std::uint64_t value) noexcept
        {
            return static_cast<std::uint16_t>((value ^ (value >> 16) ^ (value >> 32) ^ (value >> 48)) & 0xFFFFu);
        }

        /// The value of `field` in a component's JSON, or nullptr
        const nlohmann::json *FindValue(const nlohmann::json &json, const FieldInfo &field)
        {
            if (!json.is_object())
            {
                return nullptr;
            }
            const nlohmann::json *holder = &json;
            if (!field.container.empty())
            {
                const auto container = json.find(field.container);
                if (container == json.end() || !container->is_object())
                {
                    return nullptr;
                }
                holder = &*container;
            }
            const auto found = holder->find(field.name);
            return found == holder->end() ? nullptr : &*found;
        }
    }

    ObjectLayout BuildObjectLayout(GameObject &object)
    {
        ObjectLayout layout;
        layout.snapshot = SnapshotOf(object);

        Fnv1a64 hash;
        hash.String("n2net-layout-1");
        bool truncated = false;
        const auto &components = object.GetAllComponents();
        for (std::size_t index = 0; index < components.size(); ++index)
        {
            Component *const component = components[index].get();
            if (component == nullptr)
            {
                continue;
            }
            std::vector<FieldInfo> described;
            try
            {
                described = component->DescribeFields();
            }
            catch (const std::exception &)
            {
                continue;
            }
            ReplicatedComponent entry;
            entry.index = index;
            entry.firstField = layout.fields.size();
            for (FieldInfo &field : described)
            {
                if (!field.replicated || !IsReplicableKind(field.kind))
                {
                    continue;
                }
                if (layout.fields.size() >= kMaxReplicatedFieldsPerObject)
                {
                    truncated = true;
                    break;
                }
                layout.fields.push_back(std::move(field));
                layout.fieldComponent.push_back(layout.components.size());
            }
            entry.fieldCount = layout.fields.size() - entry.firstField;
            if (entry.fieldCount == 0)
            {
                continue;
            }
            hash.U32(static_cast<std::uint32_t>(index));
            hash.String(component->GetTypeName());
            hash.U32(static_cast<std::uint32_t>(entry.fieldCount));
            for (std::size_t i = entry.firstField; i < entry.firstField + entry.fieldCount; ++i)
            {
                HashFieldFormat(hash, layout.fields[i]);
            }
            layout.components.push_back(entry);
        }
        layout.hash = Fold(hash.Value());

        if (truncated)
        {
            Logger::Warn(std::format("Object '{}' has more than {} replicated fields: the rest are not replicated",
                                     object.GetName(), kMaxReplicatedFieldsPerObject));
        }
        if (layout.fields.empty())
        {
            return layout;
        }
        std::size_t worstCase = layout.MaskBytes();
        for (const FieldInfo &field : layout.fields)
        {
            worstCase += MaxEncodedBytes(field);
        }
        if (worstCase > kMaxReplicatedEntryBytes)
        {
            Logger::Error(std::format("Object '{}' replicates {} field(s) that can take {} bytes together, over the limit "
                                      "of {}: none of its fields is replicated",
                                      object.GetName(), layout.fields.size(), worstCase, kMaxReplicatedEntryBytes));
            return layout;
        }
        layout.usable = true;
        return layout;
    }

    bool ObjectLayout::IsCurrentFor(const GameObject &object) const
    {
        const auto &components = object.GetAllComponents();
        if (components.size() != snapshot.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < components.size(); ++i)
        {
            // (an empty slot matches an empty token; a component that is gone has an expired token whose control block
            // the new one at its address cannot share)
            const std::weak_ptr<const bool> current =
                components[i] ? components[i]->GetLifetimeToken() : std::weak_ptr<const bool>();
            if (!SameOwner(snapshot[i], current))
            {
                return false;
            }
        }
        return true;
    }

    const ObjectLayout &LayoutCache::Get(const NetworkId id, GameObject &object)
    {
        const auto found = _layouts.find(id.value);
        if (found != _layouts.end() && found->second.IsCurrentFor(object))
        {
            return found->second;
        }
        ObjectLayout layout = BuildObjectLayout(object);
        return (_layouts[id.value] = std::move(layout));
    }

    std::uint32_t WorldReferences::NetIdOf(const std::string &uuid) const
    {
        if (!_indexed)
        {
            _indexed = true;
            for (const auto &entry : _world.Objects())
            {
                const NetworkObject *const object = entry.second;
                _byUuid.emplace(object->GetGameObject().GetUUID().ToString(), entry.first);
            }
        }
        const auto found = _byUuid.find(uuid);
        return found == _byUuid.end() ? 0 : found->second;
    }

    std::optional<std::string> WorldReferences::UuidOf(const std::uint32_t netId) const
    {
        const NetworkObject *const object = _world.Find(NetworkId{netId});
        if (object == nullptr)
        {
            return std::nullopt;
        }
        GameObject &gameObject = object->GetGameObject();
        std::string uuid = gameObject.GetUUID().ToString();
        _resolved.emplace_back(uuid, &gameObject);
        return uuid;
    }

    void WorldReferences::RegisterResolved(ReferenceResolver &resolver) const
    {
        for (const auto &entry : _resolved)
        {
            const std::optional<Math::UUID> uuid = Math::UUID::FromString(entry.first);
            if (uuid.has_value())
            {
                resolver.RegisterGameObject(*uuid, entry.second);
            }
        }
    }

    ObjectState EncodeObjectState(const ObjectLayout &layout, GameObject &object, const IReplicationReferences *references)
    {
        ObjectState state;
        state.layoutHash = layout.hash;
        state.fields.resize(layout.fields.size());
        const auto &components = object.GetAllComponents();
        for (const ReplicatedComponent &entry : layout.components)
        {
            if (entry.index >= components.size() || components[entry.index] == nullptr)
            {
                continue;
            }
            Component &component = *components[entry.index];
            nlohmann::json values;
            try
            {
                values = component.SerializeReplicated(
                    std::span<const FieldInfo>(layout.fields.data() + entry.firstField, entry.fieldCount));
            }
            catch (const std::exception &)
            {
                continue;
            }
            for (std::size_t i = entry.firstField; i < entry.firstField + entry.fieldCount; ++i)
            {
                const nlohmann::json *const value = FindValue(values, layout.fields[i]);
                if (value != nullptr)
                {
                    // A value the codec refuses leaves the field empty: it is never sent
                    (void)EncodeField(layout.fields[i], *value, references, state.fields[i]);
                }
            }
        }
        return state;
    }

    EntryResult ApplyEntry(const ObjectLayout &layout, GameObject &object, N2Net::ByteReader &payload,
                           const WorldReferences &references, std::size_t &refused)
    {
        const std::size_t maskBytes = layout.MaskBytes();
        const std::span<const std::uint8_t> maskSpan = payload.Bytes(maskBytes);
        if (payload.Failed())
        {
            return EntryResult::Malformed;
        }
        std::uint64_t mask = 0;
        for (std::size_t i = 0; i < maskSpan.size(); ++i)
        {
            mask |= static_cast<std::uint64_t>(maskSpan[i]) << (8 * i);
        }
        const std::size_t fieldCount = layout.fields.size();
        if (mask == 0 || (fieldCount < 64 && (mask >> fieldCount) != 0))
        {
            return EntryResult::Malformed;
        }

        std::vector<nlohmann::json> perComponent(layout.components.size(), nlohmann::json::object());
        std::vector<bool> touched(layout.components.size(), false);
        bool unknownReference = false;
        for (std::size_t i = 0; i < fieldCount; ++i)
        {
            if (((mask >> i) & 1u) == 0)
            {
                continue;
            }
            const FieldInfo &field = layout.fields[i];
            nlohmann::json value;
            const DecodeStatus status = DecodeField(field, payload, &references, value);
            if (status == DecodeStatus::Malformed)
            {
                return EntryResult::Malformed;
            }
            if (status == DecodeStatus::UnknownReference)
            {
                unknownReference = true;
                continue;
            }
            nlohmann::json &target = perComponent[layout.fieldComponent[i]];
            if (field.container.empty())
            {
                target[field.name] = std::move(value);
            }
            else
            {
                if (!target.contains(field.container))
                {
                    target[field.container] = nlohmann::json::object();
                }
                target[field.container][field.name] = std::move(value);
            }
            touched[layout.fieldComponent[i]] = true;
        }
        if (payload.Remaining() != 0)
        {
            return EntryResult::Malformed;
        }
        if (unknownReference)
        {
            return EntryResult::Skipped;
        }

        ReferenceResolver resolver;
        references.RegisterResolved(resolver);
        const auto &components = object.GetAllComponents();
        for (std::size_t c = 0; c < layout.components.size(); ++c)
        {
            if (!touched[c])
            {
                continue;
            }
            const std::size_t index = layout.components[c].index;
            if (index >= components.size() || components[index] == nullptr)
            {
                ++refused;
                continue;
            }
            try
            {
                components[index]->ApplyReplicated(perComponent[c], &resolver);
            }
            catch (const std::exception &)
            {
                ++refused;
            }
        }
        resolver.ResolveAll();
        return EntryResult::Applied;
    }

    ObjectValues CaptureObjectValues(const ObjectLayout &layout, GameObject &object)
    {
        ObjectValues result;
        result.layoutHash = layout.hash;
        result.fields.resize(layout.fields.size());
        const auto &components = object.GetAllComponents();
        for (const ReplicatedComponent &entry : layout.components)
        {
            if (entry.index >= components.size() || components[entry.index] == nullptr)
            {
                continue;
            }
            nlohmann::json values;
            try
            {
                values = components[entry.index]->SerializeReplicated(
                    std::span<const FieldInfo>(layout.fields.data() + entry.firstField, entry.fieldCount));
            }
            catch (const std::exception &)
            {
                continue;
            }
            for (std::size_t i = entry.firstField; i < entry.firstField + entry.fieldCount; ++i)
            {
                if (const nlohmann::json *const value = FindValue(values, layout.fields[i]); value != nullptr)
                {
                    result.fields[i] = *value;
                }
            }
        }
        return result;
    }

    bool ApplyObjectValues(const ObjectLayout &layout, GameObject &object, const ObjectValues &values,
                           ReferenceResolver &resolver, std::size_t &refused)
    {
        if (values.layoutHash != layout.hash || values.fields.size() != layout.fields.size())
        {
            return false;
        }
        const auto &components = object.GetAllComponents();
        bool ok = true;
        for (std::size_t c = 0; c < layout.components.size(); ++c)
        {
            const ReplicatedComponent &entry = layout.components[c];
            nlohmann::json target = nlohmann::json::object();
            bool touched = false;
            for (std::size_t i = entry.firstField; i < entry.firstField + entry.fieldCount; ++i)
            {
                if (!values.fields[i].has_value())
                {
                    continue;
                }
                const FieldInfo &field = layout.fields[i];
                if (field.container.empty())
                {
                    target[field.name] = *values.fields[i];
                }
                else
                {
                    if (!target.contains(field.container))
                    {
                        target[field.container] = nlohmann::json::object();
                    }
                    target[field.container][field.name] = *values.fields[i];
                }
                touched = true;
            }
            if (!touched)
            {
                continue;
            }
            if (entry.index >= components.size() || components[entry.index] == nullptr)
            {
                ++refused;
                ok = false;
                continue;
            }
            try
            {
                components[entry.index]->ApplyReplicated(target, &resolver);
            }
            catch (const std::exception &)
            {
                ++refused;
                ok = false;
            }
        }
        resolver.ResolveAll();
        return ok;
    }
}
