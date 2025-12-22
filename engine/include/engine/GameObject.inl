#pragma once

#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/sceneManagement/Scene.hpp"

namespace N2Engine
{
    template <DerivedFromComponent T>
    T* GameObject::AddComponent()
    {
        const auto typeIndex = std::type_index(typeid(T));

        if constexpr (T::IsSingleton)
        {
            if (auto existing = GetComponent<T>())
            {
                return existing;
            }
        }

        auto component = std::make_unique<T>(*this);

        _componentMap[typeIndex] = component.get();
        _components.push_back(std::move(component));

        // if GO has already been added to scene and this component is new
        if (_scene != nullptr && SceneManager::GetCurSceneIndex() != -1)
        {
            SceneManager::GetCurSceneRef().AddComponentToAttachQueue(_componentMap[typeIndex]);
        }
        // otherwise the component will be added once added to the scene

        return static_cast<T*>(_componentMap[typeIndex]);
    }

    template <DerivedFromComponent T>
    T* GameObject::GetComponent() const
    {
        const auto typeIndex = std::type_index(typeid(T));
        if (const auto it = _componentMap.find(typeIndex); it != _componentMap.end())
        {
            return static_cast<T*>(it->second);
        }
        return nullptr;
    }

    template <DerivedFromComponent T>
    std::vector<T*> GameObject::GetComponents() const
    {
        std::vector<T*> result;
        result.reserve(_components.size()); // Optimize for common case

        for (const auto &component : _components)
        {
            if (T *castedComponent = dynamic_cast<T*>(component.get()))
            {
                result.push_back(castedComponent);
            }
        }
        return result;
    }

    template <DerivedFromComponent T>
    bool GameObject::HasComponent() const
    {
        const auto typeIndex = std::type_index(typeid(T));
        return _componentMap.contains(typeIndex);
    }

    template <DerivedFromComponent T>
    bool GameObject::RemoveComponent()
    {
        const auto typeIndex = std::type_index(typeid(T));
        return RemoveComponent(typeIndex);
    }
}
