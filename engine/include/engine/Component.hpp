#pragma once

#include <memory>

namespace N2Engine
{
    class GameObject;

    class Component
    {
        friend class GameObject;

    protected:
        GameObject &_gameObject;

        Component(GameObject &gameObject);

    public:
        // used if the attached GameObject is active as well, hierarchy is checked at that level
        bool isActive;
        GameObject &GetGameObject();

        virtual void OnAttach() {}
        virtual void OnUpdate() {}
        virtual void OnFixedUpdate() {}
        virtual void OnLateUpdate() {}
        virtual void OnDestroy() {}
        virtual void OnEnable() {}
        virtual void OnDisable() {}

        static constexpr bool IsSingleton = false;
    };
}