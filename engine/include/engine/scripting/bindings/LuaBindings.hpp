#pragma once

#include <string>
#include <vector>

#include <sol/sol.hpp>

namespace N2Engine
{
    class GameObject;
}

namespace N2Engine::Scripting
{
    class LuaRuntime;
    
    namespace Bindings
    {
        // Core systems
        void BindMath(LuaRuntime& runtime);
        void BindCore(LuaRuntime& runtime);
        void BindUtility(LuaRuntime& runtime);

        // Physics
        void BindPhysics(LuaRuntime& runtime);

        // Audio
        void BindAudio(LuaRuntime& runtime);

        // Input & Events
        void BindInput(LuaRuntime& runtime);
        void BindEvents(LuaRuntime& runtime);

        // Engine services
        void BindTime(LuaRuntime& runtime);
        void BindDebug(LuaRuntime& runtime);
        void BindApplication(LuaRuntime& runtime);
        void BindWindow(LuaRuntime& runtime);
        void BindCamera(LuaRuntime& runtime);

        // Components that scripts can add to GameObjects by name (renderers, physics, audio, LuaComponent)
        void BindComponents(LuaRuntime& runtime);

        /// GameObject:AddComponent("BoxCollider"): the component as its Lua type; raises a Lua error for unknown names
        sol::object AddComponentByName(GameObject& gameObject, const std::string& typeName, sol::this_state state);
        /// GameObject:GetComponent("BoxCollider"): the component, or nil if the object doesn't have one
        sol::object GetComponentByName(const GameObject& gameObject, const std::string& typeName, sol::this_state state);
        /// Names accepted by AddComponent/GetComponent
        std::vector<std::string> GetScriptableComponentNames();
    }
}