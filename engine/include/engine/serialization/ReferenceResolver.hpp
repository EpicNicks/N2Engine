#pragma once

#include <unordered_map>
#include <functional>
#include <memory>
#include <vector>
#include "math/UUID.hpp"

namespace N2Engine
{
    class GameObject;
    class Component;

    /**
     * Hash function for UUID - use the UUID's raw bytes
     * Since UUID is already uniformly distributed, just use part of it
     */
    struct UUIDHash
    {
        size_t operator()(const Math::UUID &uuid) const
        {
            // UUID is already a good hash, just interpret first bytes as size_t
            const auto &bytes = uuid.GetBytes();

            // Use first 8 bytes (or 4 on 32-bit systems)
            size_t hash = 0;
            for (size_t i = 0; i < sizeof(size_t) && i < bytes.size(); ++i)
            {
                hash |= static_cast<size_t>(bytes[i]) << (i * 8);
            }
            return hash;
        }
    };

    /**
     * Stores pending references that need to be resolved after full deserialization
     */
    class ReferenceResolver
    {
    public:
        using ResolveFunc = std::function<void()>;

    private:
        std::unordered_map<Math::UUID, GameObject*, UUIDHash> _gameObjectsByUUID;
        std::unordered_map<Math::UUID, Component*, UUIDHash> _componentsByUUID;
        std::vector<ResolveFunc> _pendingReferences;

    public:
        ReferenceResolver() = default;

        void RegisterGameObject(const Math::UUID &uuid, GameObject* gameObject)
        {
            _gameObjectsByUUID[uuid] = gameObject;
        }

        void RegisterComponent(const Math::UUID &uuid, Component* component)
        {
            _componentsByUUID[uuid] = component;
        }

        [[nodiscard]] GameObject* FindGameObject(const Math::UUID &uuid) const
        {
            const auto it = _gameObjectsByUUID.find(uuid);
            return (it != _gameObjectsByUUID.end()) ? it->second : nullptr;
        }

        [[nodiscard]] Component* FindComponent(const Math::UUID &uuid) const
        {
            const auto it = _componentsByUUID.find(uuid);
            return (it != _componentsByUUID.end()) ? it->second : nullptr;
        }

        void AddPendingReference(const ResolveFunc& resolver)
        {
            _pendingReferences.push_back(resolver);
        }

        void ResolveAll()
        {
            for (auto &resolver : _pendingReferences)
            {
                resolver();
            }
            _pendingReferences.clear();
        }

        void Clear()
        {
            _gameObjectsByUUID.clear();
            _componentsByUUID.clear();
            _pendingReferences.clear();
        }

        [[nodiscard]] size_t GetGameObjectCount() const { return _gameObjectsByUUID.size(); }
        [[nodiscard]] size_t GetComponentCount() const { return _componentsByUUID.size(); }
        [[nodiscard]] size_t GetPendingReferenceCount() const { return _pendingReferences.size(); }
    };
}