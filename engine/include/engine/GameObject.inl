#pragma once

#include "engine/sceneManagement/SceneManager.hpp"

namespace N2Engine
{
    template <typename T>
    inline std::shared_ptr<T> GameObject::AddComponent()
    {
        static_assert(std::is_base_of_v<Component, T>, "T must inherit from Component");

        auto typeIndex = std::type_index(typeid(T));

        if constexpr (T::IsSingleton)
        {
            if (auto existing = GetComponent<T>())
            {
                return existing;
            }
        }

        auto component = std::make_shared<T>(*this);

        _components.push_back(component);
        _componentMap[typeIndex] = component;

        if (SceneManager::GetCurSceneIndex() != -1)
        {
            SceneManager::GetCurScene().AddComponentToAttachQueue(component);
        }
        // otherwise the component will be added once added to the scene

        return component;
    }

    template <typename T>
    inline std::shared_ptr<T> GameObject::GetComponent() const
    {
        static_assert(std::is_base_of_v<Component, T>, "T must inherit from Component");

        auto typeIndex = std::type_index(typeid(T));
        auto it = _componentMap.find(typeIndex);
        if (it != _componentMap.end())
        {
            return std::static_pointer_cast<T>(it->second);
        }
        return nullptr;
    }

    template <typename T>
    inline std::vector<std::shared_ptr<T>> GameObject::GetComponents() const
    {
        static_assert(std::is_base_of_v<Component, T>, "T must inherit from Component");

        std::vector<std::shared_ptr<T>> result;
        for (const auto &component : _components)
        {
            if (auto casted = std::dynamic_pointer_cast<T>(component))
            {
                result.push_back(casted);
            }
        }
        return result;
    }

    template <typename T>
    inline bool GameObject::HasComponent() const
    {
        return GetComponent<T>() != nullptr;
    }

    template <typename T>
    inline bool GameObject::RemoveComponent()
    {
        auto typeIndex = std::type_index(typeid(T));
        return RemoveComponent(typeIndex);
    }
}