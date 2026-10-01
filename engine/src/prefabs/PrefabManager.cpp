#include <exception>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>

#include <math/UUID.hpp>

#include "engine/GameObject.hpp"
#include "engine/Logger.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/prefabs/Prefab.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

using namespace N2Engine;

namespace
{
    // Registered prefabs, as Prefab JSON
    std::unordered_map<std::string, nlohmann::json> &Registry()
    {
        static std::unordered_map<std::string, nlohmann::json> prefabs;
        return prefabs;
    }

    // Runs after references are resolved: they point at the new objects themselves, so changing the
    // UUIDs keeps every internal reference, and serializing the instance writes the new UUIDs
    void AssignFreshUUIDs(GameObject &gameObject)
    {
        gameObject.SetUUID(Math::UUID::Random());
        for (const auto &component : gameObject.GetAllComponents())
        {
            component->SetUUID(Math::UUID::Random());
        }
        for (const auto &child : gameObject.GetChildren())
        {
            AssignFreshUUIDs(*child);
        }
    }
}

std::shared_ptr<GameObject> PrefabManager::InstantiatePrefab(const nlohmann::json &prefabJson)
{
    // Prefab JSON wraps the root; bare GameObject JSON (what this function used to require) is the root
    const nlohmann::json *rootJson = &prefabJson;
    if (prefabJson.is_object())
    {
        if (const auto it = prefabJson.find("rootObject"); it != prefabJson.end())
        {
            if (it->is_null())
            {
                Logger::Error("InstantiatePrefab: the prefab has no root object");
                return nullptr;
            }
            rootJson = &*it;
        }
    }

    // The prefab's own UUIDs only key this resolver: references inside the prefab resolve to the objects
    // built here, never to another instance that was built from the same data
    ReferenceResolver resolver;
    std::shared_ptr<GameObject> root;
    try
    {
        root = GameObject::Deserialize(*rootJson, &resolver);
        resolver.ResolveAll();
    }
    catch (const std::exception &e)
    {
        Logger::Error(std::format("InstantiatePrefab: malformed prefab data: {}", e.what()));
        return nullptr;
    }
    if (!root)
    {
        Logger::Error("InstantiatePrefab: failed to build the root object");
        return nullptr;
    }

    AssignFreshUUIDs(*root);
    return root;
}

bool PrefabManager::RegisterPrefab(std::string prefabName, std::shared_ptr<GameObject> rootObject)
{
    if (prefabName.empty())
    {
        Logger::Error("RegisterPrefab: the prefab name is empty");
        return false;
    }
    if (!rootObject)
    {
        Logger::Error(std::format("RegisterPrefab: prefab '{}' has no root object", prefabName));
        return false;
    }

    nlohmann::json data = Prefab(prefabName, std::move(rootObject)).Serialize();
    auto &registry = Registry();
    if (registry.contains(prefabName))
    {
        Logger::Warn(std::format("RegisterPrefab: replacing prefab '{}'", prefabName));
    }
    registry.insert_or_assign(std::move(prefabName), std::move(data));
    return true;
}

bool PrefabManager::HasPrefab(const std::string &prefabName)
{
    return Registry().contains(prefabName);
}

bool PrefabManager::UnregisterPrefab(const std::string &prefabName)
{
    return Registry().erase(prefabName) > 0;
}

std::shared_ptr<GameObject> PrefabManager::InstantiateRegisteredPrefab(const std::string &prefabName)
{
    const auto &registry = Registry();
    const auto it = registry.find(prefabName);
    if (it == registry.end())
    {
        Logger::Error(std::format("InstantiateRegisteredPrefab: no prefab named '{}'", prefabName));
        return nullptr;
    }
    // A copy: instantiating runs component code, which could register (and rehash) another prefab
    const nlohmann::json data = it->second;
    return InstantiatePrefab(data);
}
