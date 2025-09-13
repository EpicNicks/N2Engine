#pragma once

#include <memory>

namespace N2Engine
{
    class GameObject;

    class Component
    {
        friend class GameObject;

    private:
        std::unique_ptr<GameObject> _gameObject;

    public:
        // used if the attached GameObject is active as well, hierarchy is checked at that level
        bool isActive;

        GameObject *GetGameObject() const;

        virtual void OnAttach() = 0;
        virtual void OnUpdate() = 0;
        virtual void OnDestroy() = 0;
        virtual void OnEnable() = 0;
        virtual void OnDisable() = 0;

        static constexpr bool IsSingleton = false;
    };
}