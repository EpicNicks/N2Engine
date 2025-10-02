#include "engine/GameObject.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/prefabs/Prefab.hpp"

using namespace N2Engine;

std::shared_ptr<GameObject> PrefabManager::InstantiatePrefab(const nlohmann::json &prefabJson)
{
    // TODO, should return prefab, need to determine how Scene should hold prefabs since their names should be displayed and unique
    //  how exactly they're handled should be determined later
    return GameObject::Deserialize(prefabJson);
}

void PrefabManager::RegisterPrefab(std::string prefabName, std::shared_ptr<GameObject> rootObject)
{
}