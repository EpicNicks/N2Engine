#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "engine/base/Asset.hpp"
#include "engine/physics/PhysicsTypes.hpp"
#include "engine/serialization/FieldInfo.hpp"

namespace N2Engine
{
    class GameObject;
    class Scene;
    class ReferenceResolver;

    /**
     * Base class for all components
     * Components are attached to GameObjects and provide functionality
     */
    class Component : public Base::Asset
    {
        friend class GameObject;
        friend class Scene;

    protected:
        GameObject &_gameObject;
        bool _isMarkedForDestruction = false;
        bool _isActive = true;

        explicit Component(GameObject &gameObject);

        /// Runs after SetActive changed this component's own flag. The flag fires no OnEnable/OnDisable
        /// (unlike the GameObject's), so a component that must react to it (e.g. physics) overrides this.
        virtual void OnActiveFlagChanged() {}

    private:
        // Only this component owns it, so it expires when the component is freed. Script handles keep a
        // weak_ptr to it and report the component as destroyed instead of dangling.
        std::shared_ptr<const bool> _lifetime = std::make_shared<bool>(true);

        /// The one teardown sequence for every destroy path (Destroy, RemoveComponent, scene switch):
        /// OnDisable if the component was enabled in an active hierarchy, then OnDestroy, exactly once.
        void RunDestroyCallbacks(bool objectWasActiveInHierarchy);
        // While OnDisable/OnDestroy run (before _isMarkedForDestruction is set): re-entry is a no-op, and
        // removing the component then is left to the teardown already under way
        bool _isRunningDestroyCallbacks = false;

    public:
        // Not copyable: a copy would share _lifetime, so it would expire with the original
        Component(const Component &) = delete;
        Component &operator=(const Component &) = delete;

        [[nodiscard]] GameObject& GetGameObject() const;

        // Serialization interface
        [[nodiscard]] nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json &j) override;
        virtual void Deserialize(const nlohmann::json &j, ReferenceResolver *resolver);
        [[nodiscard]] virtual std::string GetTypeName() const = 0;

        static constexpr std::string_view ResourceTypeName = "Component";
        std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        // Editor reflection (see docs/serialization.html, "Reflection"): what an inspector shows and sets

        /// The component's editable fields, which is what it saves besides the base keys (uuid, isActive). A
        /// SerializableComponent lists the members it registered; the default is none.
        [[nodiscard]] virtual std::vector<FieldInfo> DescribeFields() const { return {}; }

        /// Sets the fields `values` has keys for, and only those (a field missing from it keeps its value, even one
        /// whose deserialiser would clear it), clamping a field with a range. Throws nlohmann::json::exception for a
        /// value of the wrong type: the editor server checks a copy first, so a request is all or nothing. References
        /// are resolved through `resolver` (the caller runs its ResolveAll). The default sets nothing.
        virtual void SetEditorFields(const nlohmann::json & /*values*/, ReferenceResolver * /*resolver*/) {}

        /// The editor changed these fields (the keys of the JSON, a container's own key for one inside it), after
        /// SetEditorFields: a component with state derived from them rebuilds it here. Edit-mode components are
        /// never attached, so most have none.
        virtual void OnEditorFieldsChanged(std::span<const std::string> /*changed*/) {}

        /// `removed` is being removed from its object (in a scene opened for editing: the editor can undo it, so
        /// nothing may keep a raw pointer to it). A component that holds a pointer to another component drops it
        /// when it is `removed`. A SerializableComponent does so for its RegisterComponentRef members.
        virtual void ForgetComponent(const Component * /*removed*/) {}

        /// `removed` is being destroyed (in a scene opened for editing: the editor can undo it, so nothing may keep a
        /// raw pointer to it). A component that holds a pointer to a GameObject drops it when it is `removed`. A
        /// SerializableComponent does so for its RegisterGameObjectRef and RegisterGameObjectRefVector members.
        virtual void ForgetGameObject(const GameObject * /*removed*/) {}

        // Lifecycle methods
        virtual void OnAttach() {}

        virtual void OnUpdate() {}

        virtual void OnFixedUpdate() {}

        virtual void OnLateUpdate() {}

        virtual void OnDestroy() {}

        virtual void OnEnable() {}

        virtual void OnDisable() {}

        virtual void OnApplicationQuit() {}

        // physics collision events
        virtual void OnCollisionEnter(const Physics::Collision &collision) {}

        virtual void OnCollisionStay(const Physics::Collision &collision) {}

        virtual void OnCollisionExit(const Physics::Collision &collision) {}

        virtual void OnTriggerEnter(Physics::Trigger trigger) {}

        virtual void OnTriggerStay(Physics::Trigger trigger) {}

        virtual void OnTriggerExit(Physics::Trigger trigger) {}

        // Pointer events (Unity's OnMouse* messages), sent by Input::PointerDispatcher before Update to every
        // component of the object under the pointer (left button only); see docs/input.html#picking
        /// The pointer moved onto this object
        virtual void OnMouseEnter() {}

        /// Every frame the pointer is over this object (also the frame it entered)
        virtual void OnMouseOver() {}

        /// The pointer moved off this object
        virtual void OnMouseExit() {}

        /// The left button was pressed over this object
        virtual void OnMouseDown() {}

        /// Every frame the left button stays held after a Down on this object, wherever the pointer is
        virtual void OnMouseDrag() {}

        /// The left button was released after a Down on this object, wherever the pointer is
        virtual void OnMouseUp() {}

        /// The left button was released over this object after a Down on it (a click); comes before OnMouseUp
        virtual void OnMouseUpAsButton() {}

        [[nodiscard]] bool IsDestroyed() const;
        /// Expires when this component is freed (for references that must not dangle, e.g. from Lua)
        [[nodiscard]] std::weak_ptr<const bool> GetLifetimeToken() const { return _lifetime; }
        [[nodiscard]] bool IsActive() const;
        void SetActive(bool active);

        static constexpr bool IsSingleton = false;
    };
}
