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

        for (const auto& component : _components)
        {
            if (component == nullptr || component->IsDestroyed())
            {
                continue;
            }
            if (!includeInactive && (!component->IsActive() || !component->GetGameObject().IsActiveInHierarchy()))
            {
                continue;
            }

            if (T* casted = dynamic_cast<T*>(component))
            {
                return casted;
            }
        }

        return nullptr;
    }

    template <DerivedFromComponent T>
    std::vector<T*> Scene::FindObjectsByType(const bool includeInactive) const
    {
        if (_components.empty())
        {
            return {};
        }

        auto validComponents = _components
            | std::views::filter([includeInactive](const auto& component) {
                return component != nullptr
                    && !component->IsDestroyed()
                    && (includeInactive || (component->IsActive() && component->GetGameObject().IsActiveInHierarchy()));
            })
            | std::views::transform([](const auto& component) -> T* {
                return dynamic_cast<T*>(component);
            })
            | std::views::filter([](const T* ptr) {
                return ptr != nullptr;
            });

        return {validComponents.begin(), validComponents.end()};
    }

    template <>
    inline Rendering::Light* Scene::FindObjectByType<Rendering::Light>(const bool includeInactive) const
    {
        if (_sceneLights.empty())
        {
            return nullptr;
        }

        const auto result = std::ranges::find_if(_sceneLights, [includeInactive](const Rendering::Light* light)
        {
            return light != nullptr && !light->IsDestroyed() && (includeInactive || (light->IsActive() && light->GetGameObject().IsActiveInHierarchy()));
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
        if (_sceneLights.empty())
        {
            return {};
        }

        auto validLights = _sceneLights | std::ranges::views::filter([includeInactive](const Rendering::Light* light)
        {
            return light != nullptr
                && !light->IsDestroyed()
                && (includeInactive || (light->IsActive() && light->GetGameObject().IsActiveInHierarchy()));
        });
        return {validLights.begin(), validLights.end()};
    }
}
