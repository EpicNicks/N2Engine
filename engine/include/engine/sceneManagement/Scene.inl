#pragma once

#include "engine/sceneManagement/Scene.hpp"

namespace N2Engine
{
    template <DerivedFromComponent T>
    T* Scene::FindObjectByType(const bool includeInactive) const
    {
        T *result = nullptr;

        TraverseUntil([&](const std::shared_ptr<GameObject> &gameObject) -> bool
        {
            bool shouldProcess = includeInactive || gameObject->IsActive();

            if (shouldProcess && gameObject->HasComponent<T>())
            {
                result = gameObject->GetComponent<T>();
                return true;
            }
            return false;
        });

        return result;
    }

    template <DerivedFromComponent T>
    std::vector<T*> Scene::FindObjectsByType(const bool includeInactive) const
    {
        std::vector<T*> result;

        if (includeInactive)
        {
            TraverseAll([&](const std::shared_ptr<GameObject> &gameObject)
            {
                if (gameObject->HasComponent<T>())
                {
                    result.push_back(gameObject->GetComponent<T>());
                }
            });
        }
        else
        {
            TraverseAllActive([&](const std::shared_ptr<GameObject> &gameObject)
            {
                if (gameObject->HasComponent<T>())
                {
                    result.push_back(gameObject->GetComponent<T>());
                }
            });
        }

        return result;
    }

    template <>
    inline Rendering::Light* Scene::FindObjectByType<Rendering::Light>(bool includeInactive) const
    {
        if (_sceneLights.empty())
            return nullptr;

        // Return first light (or add logic to check if active)
        return _sceneLights[0];
    }

    template <>
    inline std::vector<Rendering::Light*> Scene::FindObjectsByType<Rendering::Light>(bool includeInactive) const
    {
        return _sceneLights;
    }
}
