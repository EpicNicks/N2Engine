#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>

namespace N2Engine
{

    class GameObject;

    struct Scene
    {
    private:
        std::vector<std::shared_ptr<GameObject>> _rootGameObjects;

    public:
        std::string sceneName;

        void AddRootGameObject(std::shared_ptr<GameObject> gameObject);

        bool RemoveRootGameObject(std::shared_ptr<GameObject> gameObject);

        bool DestroyGameObject(std::shared_ptr<GameObject> gameObject);

        const std::vector<std::shared_ptr<GameObject>> &GetRootGameObjects() const { return _rootGameObjects; }
        size_t GetRootGameObjectCount() const { return _rootGameObjects.size(); }

        void TraverseAll(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        void TraverseAllActive(std::function<void(std::shared_ptr<GameObject>)> callback) const;
        bool TraverseUntil(std::function<bool(std::shared_ptr<GameObject>)> callback) const;

        std::shared_ptr<GameObject> FindGameObject(const std::string &name) const;
        std::vector<std::shared_ptr<GameObject>> FindGameObjectsByTag(const std::string &tag) const;

        std::vector<std::shared_ptr<GameObject>> GetAllGameObjects() const;

        template <typename T>
        std::shared_ptr<T> FindObjectByType(bool includeInactive) const;
        template <typename T>
        std::vector<std::shared_ptr<T>> Scene::FindObjectsByType(bool includeInactive) const;

        void Update();
        void Clear();

    private:
        void TraverseGameObjectRecursive(std::shared_ptr<GameObject> gameObject,
                                         std::function<void(std::shared_ptr<GameObject>)> callback,
                                         bool onlyActive = false) const;
        bool TraverseGameObjectUntil(std::shared_ptr<GameObject> gameObject,
                                     std::function<bool(std::shared_ptr<GameObject>)> callback) const;
    };

    template <typename T>
    std::shared_ptr<T> Scene::FindObjectByType(bool includeInactive) const
    {
        std::shared_ptr<T> result = nullptr;

        TraverseUntil([&](std::shared_ptr<GameObject> gameObject) -> bool
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

    template <typename T>
    std::vector<std::shared_ptr<T>> Scene::FindObjectsByType(bool includeInactive) const
    {
        std::vector<std::shared_ptr<T>> result;

        if (includeInactive)
        {
            TraverseAll([&](std::shared_ptr<GameObject> gameObject)
                        {
                if (gameObject->HasComponent<T>())
                {
                    result.push_back(gameObject->GetComponent<T>());
                } });
        }
        else
        {
            TraverseAllActive([&](std::shared_ptr<GameObject> gameObject)
                              {
                if (gameObject->HasComponent<T>())
                {
                    result.push_back(gameObject->GetComponent<T>());
                } });
        }

        return result;
    }
}