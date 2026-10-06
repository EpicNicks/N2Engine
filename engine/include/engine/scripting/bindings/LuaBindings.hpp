#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sol/sol.hpp>
#include <text/TextLayout.hpp>

#include "engine/scripting/LuaHandles.hpp"

namespace N2Engine::Text
{
    class Font;
}

namespace N2Engine::Rendering
{
    class Texture;
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
        // Screen-space UI: RectTransform, Canvas, Image, UIText, Rect and the UI table
        void BindUI(LuaRuntime& runtime);

        /// Text alignments by their scene-file names ("Left", "Center", "Right"; "Top", "Middle", "Bottom",
        /// "Baseline"), for TextRenderer and UIText. Parse* throw (a Lua error at the call site) for any other.
        Text::HorizontalAlign ParseHorizontalAlign(const std::string& name);
        Text::VerticalAlign ParseVerticalAlign(const std::string& name);
        std::string AlignName(Text::HorizontalAlign align);
        std::string AlignName(Text::VerticalAlign align);
        /// A font file through Resources, e.g. "res://fonts/Title.ttf"; throws "<caller>: can't load font ..."
        /// if it doesn't load
        std::shared_ptr<Text::Font> LoadFontOrThrow(const std::string& path, std::string_view caller);
        /// An image file through Resources, e.g. "res://ui/icon.png"; throws "<caller>: can't load texture ..."
        /// if it doesn't load
        std::shared_ptr<Rendering::Texture> LoadTextureOrThrow(const std::string& path, std::string_view caller);
        /// What scripts see as a texture's path: its res:// (or user://) path for a project asset, else the file
        /// it was loaded from, else "" (a texture made at runtime)
        std::string TexturePathForLua(const Rendering::Texture& texture);

        /// GameObject:AddComponent("BoxCollider"): the component as its Lua type; raises a Lua error for unknown names
        sol::object AddComponentByName(GameObject& gameObject, const std::string& typeName, sol::this_state state);
        /// GameObject:GetComponent("BoxCollider"): the component, or nil if the object doesn't have one
        sol::object GetComponentByName(const GameObject& gameObject, const std::string& typeName, sol::this_state state);
        /// Names accepted by AddComponent/GetComponent
        std::vector<std::string> GetScriptableComponentNames();
        /// A component as its concrete Lua type when it's one of those, else as Component
        sol::object ComponentToLua(Component& component, lua_State* state);
        /// Lua __eq for components: true for two handles to the same component, whatever their Lua types
        bool SameComponent(const sol::object& a, const sol::object& b);

        /// The OnCollision*/OnTrigger* argument: a copy whose objects are handles, so a script can keep it
        sol::object CollisionToLua(const Physics::Collision& collision, lua_State* state);
        sol::object TriggerToLua(const Physics::Trigger& trigger, lua_State* state);

        /// Registers a component's Lua type, on ComponentRef<T>: the given members plus Component's own
        template <typename T, typename... Members>
        void BindComponentType(sol::state& lua, const char* name, Members&&... members)
        {
            using Ref = ComponentRef<T>;
            Ref::s_luaName = name;
            lua.new_usertype<Ref>(
                name,
                sol::no_constructor,
                sol::meta_function::equal_to, &SameComponent,

                "IsValid", [](const Ref& ref) { return ref.IsValid(); },
                // Unlike the other methods, fine to call on a destroyed component
                "IsDestroyed", [](const Ref& ref) { return !ref.IsValid(); },
                "IsActive", Forward<Ref, &Component::IsActive>(),
                "SetActive", Forward<Ref, &Component::SetActive>(),
                "GetGameObject", [](const Ref& ref) { return GameObjectRef(ref.Pin()->GetGameObject()); },

                std::forward<Members>(members)...
            );
        }
    }
}