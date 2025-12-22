#pragma once

#include <unordered_map>
#include <functional>
#include <vector>

#include <math/UUID.hpp>

#include "engine/common/UUIDHash.hpp"

namespace N2Engine
{
    class GameObject;
    class Component;

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