#pragma once

#include <memory>

namespace N2Engine
{
    class GameObject;
    class Scene;

    class Component
    {
        friend class GameObject;
        friend class Scene;

    protected:
        GameObject &_gameObject;              // will be resolved by hierarchy
        bool _isMarkedForDestruction = false; // not persistent state
        bool _isActive = true;

        Component(GameObject &gameObject);

    public:
        // used if the attached GameObject is active as well, hierarchy is checked at that level
        GameObject &GetGameObject();

        virtual void OnAttach() {}
        virtual void OnUpdate() {}
        virtual void OnFixedUpdate() {}
        virtual void OnLateUpdate() {}
        virtual void OnDestroy() {}
        virtual void OnEnable() {}
        virtual void OnDisable() {}
        virtual void OnApplicationQuit() {}

        bool IsDestroyed();
        bool GetIsActive();

        static constexpr bool IsSingleton = false;
    };
}