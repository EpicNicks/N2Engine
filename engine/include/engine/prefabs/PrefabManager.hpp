#pragma once

#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace N2Engine
{
    class GameObject;
    class ReferenceResolver;

    namespace PrefabManager
    {
        /// Builds a new object tree from prefab data. Accepts Prefab JSON ({"name", "rootObject"}, what
        /// Prefab::Serialize writes) or the bare GameObject JSON of its root (the "rootObject" value).
        /// Every GameObject and component in the instance gets a fresh UUID, so instantiating twice never
        /// gives two objects the same UUID. References between objects/components of the prefab are resolved
        /// to the new instance's own objects; references to anything outside the prefab come back null.
        /// The instance is in no scene: add it to one (AddRootGameObject/AddChild) to attach it.
        /// Malformed data logs an error and returns nullptr; no exception escapes.
        std::shared_ptr<GameObject> InstantiatePrefab(const nlohmann::json &prefabJson);

        /// InstantiatePrefab, except that references to anything outside the prefab are kept instead of nulled:
        /// they are looked up in `outside`, which knows the objects and components of the scene the copy is made
        /// for (every GameObject and component registered under its UUID). A reference `outside` doesn't know
        /// still comes back null. This is what duplicating an object in a scene needs: a copy's reference to an
        /// object that isn't part of it keeps pointing at that object, as in Unity. `outside` must stay alive
        /// for the call only.
        std::shared_ptr<GameObject> InstantiatePrefab(const nlohmann::json &prefabJson,
                                                      const ReferenceResolver &outside);

        /// Stores a snapshot of rootObject (its hierarchy as serialized now) as the prefab prefabName,
        /// replacing (with a warning) a prefab already registered under that name. The object itself is not
        /// kept or changed: later edits to it don't affect the prefab. False (logged) for an empty name or a
        /// null root.
        bool RegisterPrefab(std::string prefabName, std::shared_ptr<GameObject> rootObject);
        /// Whether a prefab is registered under this name
        bool HasPrefab(const std::string &prefabName);
        /// Removes a registered prefab; false if there was none with this name
        bool UnregisterPrefab(const std::string &prefabName);
        /// InstantiatePrefab on the registered prefab's data; nullptr (logged) if no prefab has this name
        std::shared_ptr<GameObject> InstantiateRegisteredPrefab(const std::string &prefabName);
    }
}
