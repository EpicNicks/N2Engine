#pragma once

#include "engine/GameObject.hpp"

namespace N2Engine
{
    template <typename T>
    std::shared_ptr<T> Scene::FindObjectByType(bool includeInactive) const
    {
        std::shared_ptr<T> result = nullptr;

        TraverseUntil([&](std::shared_ptr<GameObject> gameObject) -> bool
                      {
            bool shouldProcess = includeInactive || gameObject->IsActive();
            
            if (shouldProcess && gameObject->template HasComponent<T>())
            {
                result = gameObject->template GetComponent<T>();
                return true;
            }
            return false; });

        return result;
    }

    template <typename T>
    std::vector<std::shared_ptr<T>> Scene::FindObjectsByType(bool includeInactive) const
    {
        std::vector<std::shared_ptr<T>> result;

        if (includeInactive)
        {
            TraverseAll([&](std::shared_ptr<GameObject> gameObject)
                        {
                if (gameObject->template HasComponent<T>())
                {
                    result.push_back(gameObject->template GetComponent<T>());
                } });
        }
        else
        {
            TraverseAllActive([&](std::shared_ptr<GameObject> gameObject)
                              {
                if (gameObject->template HasComponent<T>())
                {
                    result.push_back(gameObject->template GetComponent<T>());
                } });
        }

        return result;
    }
}