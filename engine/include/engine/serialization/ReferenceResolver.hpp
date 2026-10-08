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
        const ReferenceResolver *_fallback = nullptr;

    public:
        ReferenceResolver() = default;

        /**
         * Lookups that find nothing registered here are answered by `fallback` (not owned: it must outlive every
         * ResolveAll that uses this). For building part of a scene, such as a copy of one object's subtree, whose
         * references to the rest of the scene should stay: the fallback knows the scene's objects and components.
         * nullptr (the default) means ids that aren't registered here resolve to nothing.
         */
        void SetFallback(const ReferenceResolver *fallback)
        {
            _fallback = fallback;
        }

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
            if (it != _gameObjectsByUUID.end())
            {
                return it->second;
            }
            return _fallback != nullptr ? _fallback->FindGameObject(uuid) : nullptr;
        }

        [[nodiscard]] Component* FindComponent(const Math::UUID &uuid) const
        {
            const auto it = _componentsByUUID.find(uuid);
            if (it != _componentsByUUID.end())
            {
                return it->second;
            }
            return _fallback != nullptr ? _fallback->FindComponent(uuid) : nullptr;
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