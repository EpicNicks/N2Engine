#include <exception>
#include <format>
#include <optional>
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

    using UUIDMap = std::unordered_map<std::string, std::string>;

    // The canonical text of a UUID string (FromString accepts either case), or nullopt if it isn't one
    std::optional<std::string> CanonicalUUID(const nlohmann::json &value)
    {
        if (!value.is_string())
        {
            return std::nullopt;
        }
        const auto &text = value.get_ref<const std::string &>();
        if (text.size() != 36)
        {
            return std::nullopt;
        }
        if (const auto uuid = Math::UUID::FromString(text))
        {
            return uuid->ToString();
        }
        return std::nullopt;
    }

    // Gives every GameObject and component id in the prefab's GameObject JSON a fresh UUID (old -> new)
    void CollectIds(const nlohmann::json &gameObject, UUIDMap &ids)
    {
        if (!gameObject.is_object())
        {
            return;
        }
        auto addId = [&ids](const nlohmann::json &owner)
        {
            if (!owner.is_object())
            {
                return;
            }
            if (const auto it = owner.find("uuid"); it != owner.end())
            {
                if (auto old = CanonicalUUID(*it); old && *old != Math::UUID::ZERO.ToString())
                {
                    ids.try_emplace(std::move(*old), Math::UUID::Random().ToString());
                }
            }
        };
        addId(gameObject);
        if (const auto components = gameObject.find("components"); components != gameObject.end() && components->is_array())
        {
            for (const auto &component : *components)
            {
                if (component.is_object())
                {
                    if (const auto data = component.find("data"); data != component.end())
                    {
                        addId(*data);
                    }
                }
            }
        }
        if (const auto children = gameObject.find("children"); children != gameObject.end() && children->is_array())
        {
            for (const auto &child : *children)
            {
                CollectIds(child, ids);
            }
        }
    }

    // Rewrites the prefab data in place: every string that is one of the prefab's ids (the ids themselves,
    // GameObject/component reference members, Lua "$ref" fields) becomes the new id. A Lua "$ref" to
    // anything outside the prefab becomes null: the template may be instantiated where that object isn't.
    // Other UUID strings (assets, scripts) are not ids of the prefab and are left alone.
    void Renumber(nlohmann::json &value, const UUIDMap &ids)
    {
        if (value.is_string())
        {
            if (const auto canonical = CanonicalUUID(value))
            {
                if (const auto it = ids.find(*canonical); it != ids.end())
                {
                    value = it->second;
                }
            }
            return;
        }
        if (value.is_object())
        {
            if (const auto ref = value.find("$ref"); ref != value.end() && ref->is_string())
            {
                const auto canonical = CanonicalUUID(*ref);
                if (!canonical || !ids.contains(*canonical))
                {
                    *ref = nullptr;
                }
            }
            for (auto &item : value.items())
            {
                Renumber(item.value(), ids);
            }
            return;
        }
        if (value.is_array())
        {
            for (auto &item : value)
            {
                Renumber(item, ids);
            }
        }
    }

    // Objects or components the data gave no usable id (missing or invalid) would all share UUID::ZERO
    void ReplaceZeroUUIDs(GameObject &gameObject)
    {
        if (gameObject.GetUUID() == Math::UUID::ZERO)
        {
            gameObject.SetUUID(Math::UUID::Random());
        }
        for (const auto &component : gameObject.GetAllComponents())
        {
            if (component->GetUUID() == Math::UUID::ZERO)
            {
                component->SetUUID(Math::UUID::Random());
            }
        }
        for (const auto &child : gameObject.GetChildren())
        {
            ReplaceZeroUUIDs(*child);
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

    // Renumber in the data, before anything is built: every id, every reference to one (pointer members
    // and Lua "$ref" fields alike) and so the saved form of the instance all use the new UUIDs. The
    // resolver then links references to the objects built here, never to the prefab's source objects.
    ReferenceResolver resolver;
    std::shared_ptr<GameObject> root;
    try
    {
        nlohmann::json data = *rootJson;
        UUIDMap ids;
        CollectIds(data, ids);
        Renumber(data, ids);
        root = GameObject::Deserialize(data, &resolver);
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

    ReplaceZeroUUIDs(*root);
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
