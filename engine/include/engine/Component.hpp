#pragma once

namespace N2Engine
{
    class Component
    {
    public:
        // used if the attached GameObject is active as well, hierarchy is checked at that level
        bool isActive;
        virtual void OnAttach() = 0;
        virtual void OnUpdate() = 0;
        virtual void OnDestroy() = 0;
        virtual void OnEnable() = 0;
        virtual void OnDisable() = 0;
    };
}