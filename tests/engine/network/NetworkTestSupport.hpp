#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <math/UUID.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/sceneManagement/Scene.hpp"

// Helpers shared by the world-layer tests (netcode stage B1): scenes whose GameObjects have chosen UUIDs and a
// NetworkObject each, and prefabs that are registered for one test and removed after it.

namespace N2Engine::Network::Testing
{
    /// The UUID 00000000-0000-4000-8000-<n as 12 hex digits>: n decides the order scene objects are matched in
    inline Math::UUID TestUuid(const std::uint32_t n)
    {
        char text[40];
        std::snprintf(text, sizeof text, "00000000-0000-4000-8000-%012x", static_cast<unsigned>(n));
        return Math::UUID::FromString(text).value();
    }

    /// A scene with one root object per entry of `uuids`, in that order, named "Object<n>", UUID TestUuid(n); each has a
    /// NetworkObject unless `withNetworkObject` is false
    inline std::unique_ptr<Scene> MakeScene(const std::string &name, const std::vector<std::uint32_t> &uuids,
                                            const bool withNetworkObject = true)
    {
        std::unique_ptr<Scene> scene = Scene::Create(name);
        for (const std::uint32_t n : uuids)
        {
            const GameObject::Ptr gameObject = GameObject::Create("Object" + std::to_string(n));
            gameObject->SetUUID(TestUuid(n));
            if (withNetworkObject)
            {
                gameObject->AddComponent<NetworkObject>();
            }
            scene->AddRootGameObject(gameObject);
        }
        return scene;
    }

    /// The root object of `scene` with the UUID TestUuid(n); nullptr if there is none
    inline GameObject::Ptr FindByNumber(Scene &scene, const std::uint32_t n)
    {
        return scene.FindGameObjectByUUID(TestUuid(n));
    }

    /// A prefab registered with PrefabManager for the life of this object. `Shape` decides where its NetworkObjects are.
    class ScopedPrefab
    {
    public:
        enum class Shape
        {
            /// One NetworkObject, on the root: what a spawnable prefab must be
            OneOnRoot,
            /// No NetworkObject at all
            None,
            /// One NetworkObject, on a child
            OneOnChild,
            /// One on the root and one on a child
            TwoInTree,
        };

        explicit ScopedPrefab(std::string name, const Shape shape = Shape::OneOnRoot) : _name(std::move(name))
        {
            const GameObject::Ptr root = GameObject::Create("Prefab_" + _name);
            const GameObject::Ptr child = GameObject::Create("Child");
            switch (shape)
            {
            case Shape::OneOnRoot:
                root->AddComponent<NetworkObject>();
                break;
            case Shape::None:
                break;
            case Shape::OneOnChild:
                child->AddComponent<NetworkObject>();
                break;
            case Shape::TwoInTree:
                root->AddComponent<NetworkObject>();
                child->AddComponent<NetworkObject>();
                break;
            }
            if (shape != Shape::None)
            {
                root->AddChild(child, false);
            }
            _registered = PrefabManager::RegisterPrefab(_name, root);
        }

        ~ScopedPrefab() { PrefabManager::UnregisterPrefab(_name); }
        ScopedPrefab(const ScopedPrefab &) = delete;
        ScopedPrefab &operator=(const ScopedPrefab &) = delete;

        [[nodiscard]] const std::string &Name() const { return _name; }
        [[nodiscard]] bool Registered() const { return _registered; }

    private:
        std::string _name;
        bool _registered = false;
    };
}
