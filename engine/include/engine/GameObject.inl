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
        T *added = component.get();

        // The first component of a type is the one GetComponent returns (Unity's rule). This used to
        // repoint the map at the newest, leaving earlier ones unreachable by GetComponent/RemoveComponent.
        _componentMap.emplace(typeIndex, added);
        _components.push_back(std::move(component));

        // if GO has already been added to a scene, attach there (its own scene, not the loaded one)
        if (_scene != nullptr)
        {
            _scene->AddComponentToAttachQueue(added);
        }
        // otherwise the component will be added once added to the scene

        return added;
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
