#pragma once

#include <nlohmann/json.hpp>
#include <memory>
#include <filesystem>

namespace N2Engine
{
    namespace PrefabManager
    {
        std::shared_ptr<class GameObject> InstantiatePrefab(const nlohmann::json &prefabJson);
        void RegisterPrefab(std::string prefabName, std::shared_ptr<class GameObject> rootObject);
    };
}
