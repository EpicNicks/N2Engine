#pragma once

#include "engine/sceneManagement/Scene.hpp"

namespace N2Engine
{
    template <DerivedFromComponent T>
    T* Scene::FindObjectByType(const bool includeInactive) const
    {
        if (_components.empty())
        {
            return nullptr;
        }

        const auto firstFoundComponent = std::ranges::find_if(_components, [includeInactive](const auto& component)
        {
            return component != nullptr && !component->IsDestroyed() && (includeInactive || (component->IsActive() && component->GetGameObject().IsActiveInHierarchy()));
        });

        if (firstFoundComponent == _components.end())
        {
            return nullptr;
        }
        return *firstFoundComponent;
    }

    template <DerivedFromComponent T>
    std::vector<T*> Scene::FindObjectsByType(const bool includeInactive) const
    {
        if (_components.empty())
        {
            return nullptr;
        }

        const auto foundComponents = _components | std::ranges::views::filter([includeInactive](const auto& component)
        {
            return component != nullptr && !component->IsDestroyed() && (includeInactive || (component->IsActive() && component->GetGameObject().IsActiveInHierarchy()));
        });
        return {foundComponents.begin(), foundComponents.end()};
    }

    template <>
    inline Rendering::Light* Scene::FindObjectByType<Rendering::Light>(const bool includeInactive) const
    {
        if (_sceneLights.empty())
        {
            return nullptr;
        }

        if (includeInactive)
        {
            return _sceneLights[0];
        }

        const auto result = std::ranges::find_if(_sceneLights, [](const Rendering::Light* light)
        {
            return light != nullptr && light->IsActive() && light->GetGameObject().IsActiveInHierarchy() && !light->IsDestroyed();
        });

        if (result == _sceneLights.end())
        {
            return nullptr;
        }
        return *result;
    }

    template <>
    inline std::vector<Rendering::Light*> Scene::FindObjectsByType<Rendering::Light>(const bool includeInactive) const
    {
        if (includeInactive)
        {
            return _sceneLights;
        }

        auto activeLights = _sceneLights | std::ranges::views::filter([](const Rendering::Light* light)
        {
            return light != nullptr && light->IsActive() && light->GetGameObject().IsActiveInHierarchy() && !light->IsDestroyed();
        });
        return {activeLights.begin(), activeLights.end()};
    }
}
