#pragma once

#include "engine/GameObject.hpp"

namespace N2Engine
{
    template <DerivedFromComponent T>
    std::shared_ptr<T> Scene::FindObjectByType(const bool includeInactive) const
    {
        std::shared_ptr<T> result = nullptr;

        TraverseUntil([&](const std::shared_ptr<GameObject>& gameObject) -> bool
                      {
            bool shouldProcess = includeInactive || gameObject->IsActive();
            
            if (shouldProcess && gameObject->HasComponent<T>())
            {
                result = gameObject->GetComponent<T>();
                return true;
            }
            return false; });

        return result;
    }

    template <DerivedFromComponent T>
    std::vector<std::shared_ptr<T>> Scene::FindObjectsByType(const bool includeInactive) const
    {
        std::vector<std::shared_ptr<T>> result;

        if (includeInactive)
        {
            TraverseAll([&](const std::shared_ptr<GameObject>& gameObject)
                        {
                if (gameObject->HasComponent<T>())
                {
                    result.push_back(gameObject->template GetComponent<T>());
                } });
        }
        else
        {
            TraverseAllActive([&](const std::shared_ptr<GameObject>& gameObject)
                              {
                if (gameObject->HasComponent<T>())
                {
                    result.push_back(gameObject->template GetComponent<T>());
                } });
        }

        return result;
    }
}