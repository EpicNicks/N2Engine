#pragma once

#include "engine/GameObject.hpp"
#include "engine/sceneManagement/Scene.hpp"

namespace N2Engine
{
    template <typename T>
    std::shared_ptr<T> Scene::FindObjectByType(const bool includeInactive) const
    {
        std::shared_ptr<T> result = nullptr;

        TraverseUntil([&](const std::shared_ptr<GameObject>& gameObject) -> bool
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
    std::vector<std::shared_ptr<T>> Scene::FindObjectsByType(const bool includeInactive) const
    {
        std::vector<std::shared_ptr<T>> result;

        if (includeInactive)
        {
            TraverseAll([&](const std::shared_ptr<GameObject>& gameObject)
                        {
                if (gameObject->template HasComponent<T>())
                {
                    result.push_back(gameObject->template GetComponent<T>());
                } });
        }
        else
        {
            TraverseAllActive([&](const std::shared_ptr<GameObject>& gameObject)
                              {
                if (gameObject->template HasComponent<T>())
                {
                    result.push_back(gameObject->template GetComponent<T>());
                } });
        }

        return result;
    }
}