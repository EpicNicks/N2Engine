#pragma once

#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <sol/sol.hpp>

#include "engine/Component.hpp"
#include "engine/GameObject.hpp"

namespace N2Engine
{
    class Positionable;
    class Scene;
}

namespace N2Engine::Input
{
    class ActionMap;
    class InputAction;
}

// What Lua holds instead of raw engine pointers. A script can keep any of these (in `self`, a global or a
// closure) past the object's lifetime: using it afterwards raises a Lua error ("attempt to use a destroyed
// Rigidbody") instead of touching freed memory, and IsValid() reports whether it's still usable.
//
// A handle works until its object's teardown has finished (GameObject::IsTornDown): throughout the
// OnDisable/OnDestroy callbacks of a destroy, a scene switch or RemoveComponent, handles to the object, its
// components and its siblings being torn down with it all still work; afterwards they all fail. Scenes and
// input objects (LifetimeRef) work until they're freed.
namespace N2Engine::Scripting
{
    /// A GameObject in Lua. Doesn't keep the object alive, except for a handle a script created
    /// (GameObject.Create) or detached (RemoveChild, RemoveRootGameObject) the object with, since the script
    /// may then be its only owner. Ownership is per handle: other handles to the same object stay weak.
    class GameObjectRef
    {
    public:
        /// Doesn't own the object (anything the engine already owns: scene objects, collision partners, ...)
        explicit GameObjectRef(const GameObject &gameObject)
            : _object(std::const_pointer_cast<GameObject>(gameObject.weak_from_this().lock()))
        {
        }

        explicit GameObjectRef(const GameObject::Ptr &gameObject)
            : _object(gameObject)
        {
        }

        /// Also owns the object: for GameObject.Create, before any scene or parent holds it
        static GameObjectRef Owning(GameObject::Ptr gameObject)
        {
            GameObjectRef ref(gameObject);
            ref._owned = std::move(gameObject);
            return ref;
        }

        /// Keeps the object alive from now on, e.g. once a script detached it from its only owner.
        /// Caveat: a Lua-owned object that never joins a scene, and whose own script keeps this handle
        /// (e.g. in self), is a reference cycle through the Lua registry and is never freed. (The LuaComponent
        /// holds its script table by registry reference, which Lua's GC treats as a root, so it can't see the
        /// cycle; scripts should use the non-owning self.gameObject instead.)
        void TakeOwnership() { _owned = _object.lock(); }

        /// Back to a weak reference, e.g. once a scene or parent holds the object
        void ReleaseOwnership() { _owned.reset(); }

        [[nodiscard]] bool IsValid() const
        {
            const GameObject::Ptr object = _object.lock();
            return object && !object->IsTornDown();
        }

        /// The object even if it's destroyed, or null once it's freed
        [[nodiscard]] GameObject::Ptr Lock() const { return _object.lock(); }

        /// The live object, held for the duration of a call. Throws once it's torn down; sol turns that
        /// into a Lua error at the call site.
        [[nodiscard]] GameObject::Ptr Pin() const
        {
            GameObject::Ptr object = _object.lock();
            if (!object || object->IsTornDown())
            {
                throw std::runtime_error("attempt to use a destroyed GameObject");
            }
            return object;
        }

        /// Same object, also after it's freed (compares ownership, not addresses, which can be reused)
        [[nodiscard]] bool RefersTo(const GameObjectRef &other) const
        {
            return !_object.owner_before(other._object) && !other._object.owner_before(_object);
        }

    private:
        std::weak_ptr<GameObject> _object;
        GameObject::Ptr _owned;
    };

    /// The non-template part of ComponentRef: validity and identity, whatever the component type
    class ComponentRefBase
    {
    public:
        [[nodiscard]] bool IsValid() const { return static_cast<bool>(LockOwner()); }

        [[nodiscard]] bool RefersTo(const ComponentRefBase &other) const
        {
            return !_lifetime.owner_before(other._lifetime) && !other._lifetime.owner_before(_lifetime);
        }

    protected:
        explicit ComponentRefBase(const Component &component)
            : _owner(component.GetGameObject().weak_from_this()), _lifetime(component.GetLifetimeToken())
        {
        }

        /// The owning GameObject while the component is usable (not freed, owner not torn down), else null
        [[nodiscard]] GameObject::Ptr LockOwner() const
        {
            GameObject::Ptr owner = _owner.lock();
            if (!owner || _lifetime.expired() || owner->IsTornDown())
            {
                return nullptr;
            }
            return owner;
        }

        std::weak_ptr<GameObject> _owner;
        std::weak_ptr<const bool> _lifetime;
    };

    /// A component in Lua, as its concrete type. Invalid once it's freed (RemoveComponent) or its GameObject
    /// is torn down (destroyed, or its scene unloaded).
    template <typename T>
    class ComponentRef : public ComponentRefBase
    {
    public:
        /// The type's Lua name, for errors; set when the type is bound (BindComponentType)
        static inline std::string_view s_luaName = "Component";

        explicit ComponentRef(T &component)
            : ComponentRefBase(component), _typed(&component)
        {
        }

        /// The live component. The returned pointer also keeps its GameObject (so the component) alive, e.g.
        /// through a call that re-enters Lua, where a GC could drop the GameObject's last owner; hold it for
        /// the whole call. Throws when the component is gone; sol turns that into a Lua error at the call site.
        [[nodiscard]] std::shared_ptr<T> Pin() const
        {
            GameObject::Ptr owner = LockOwner();
            if (!owner)
            {
                throw std::runtime_error(std::format("attempt to use a destroyed {}", s_luaName));
            }
            return std::shared_ptr<T>(std::move(owner), _typed);
        }

    private:
        T *_typed;
    };

    /// A GameObject's Positionable in Lua (it lives exactly as long as its GameObject)
    class PositionableRef
    {
    public:
        explicit PositionableRef(const GameObject &owner)
            : _owner(owner)
        {
        }

        [[nodiscard]] bool IsValid() const { return _owner.IsValid(); }

        /// The live Positionable; the returned pointer also keeps its GameObject alive for the call
        [[nodiscard]] std::shared_ptr<Positionable> Pin() const
        {
            const GameObject::Ptr owner = _owner.Lock();
            if (!owner || owner->IsTornDown() || !owner->GetPositionable())
            {
                throw std::runtime_error("attempt to use the Positionable of a destroyed GameObject");
            }
            return std::shared_ptr<Positionable>(owner, owner->GetPositionable());
        }

        [[nodiscard]] bool RefersTo(const PositionableRef &other) const { return _owner.RefersTo(other._owner); }

    private:
        GameObjectRef _owner;
    };

    /// An engine object that isn't part of a GameObject, and that its owner frees whenever it likes: a Scene
    /// (freed on a scene switch), an ActionMap (replaced or reloaded) or an InputAction (replaced, removed, or
    /// its map freed). Valid until the object is freed, tracked by the lifetime token it owns
    /// (GetLifetimeToken). One replaced from an input callback is freed when that input update ends.
    template <typename T>
    class LifetimeRef
    {
    public:
        /// The type's Lua name, for errors; set when the type is bound
        static inline std::string_view s_luaName = "object";

        explicit LifetimeRef(T &object)
            : _object(&object), _lifetime(object.GetLifetimeToken())
        {
        }

        [[nodiscard]] bool IsValid() const { return !_lifetime.expired(); }

        /// The live object. A plain pointer is enough: none of these can be freed during a call on it from
        /// Lua (scene switches, and the end of an input update, never run inside a script call). Throws once
        /// it's freed; sol turns that into a Lua error at the call site.
        [[nodiscard]] T *Pin() const
        {
            if (_lifetime.expired())
            {
                throw std::runtime_error(std::format("attempt to use a destroyed {}", s_luaName));
            }
            return _object;
        }

        /// Same object, also after it's freed (compares tokens, not addresses, which can be reused)
        [[nodiscard]] bool RefersTo(const LifetimeRef &other) const
        {
            return !_lifetime.owner_before(other._lifetime) && !other._lifetime.owner_before(_lifetime);
        }

    private:
        T *_object;
        std::weak_ptr<const bool> _lifetime;
    };

    using SceneRef = LifetimeRef<Scene>;
    using ActionMapRef = LifetimeRef<Input::ActionMap>;
    using InputActionRef = LifetimeRef<Input::InputAction>;

    /// Lua __eq for a handle type: true for two handles to the same object
    template <typename Handle>
    bool SameRef(const sol::object &a, const sol::object &b)
    {
        return a.is<Handle>() && b.is<Handle>() && a.as<Handle>().RefersTo(b.as<Handle>());
    }

    namespace Detail
    {
        template <typename T>
        struct IsSmartPointer : std::false_type
        {
        };

        template <typename T>
        struct IsSmartPointer<std::shared_ptr<T>> : std::true_type
        {
        };

        template <typename T>
        struct IsSmartPointer<std::weak_ptr<T>> : std::true_type
        {
        };

        template <typename T, typename D>
        struct IsSmartPointer<std::unique_ptr<T, D>> : std::true_type
        {
        };

        // Engine objects must reach Lua as handles, never directly (by pointer, smart pointer or reference)
        template <typename R>
        constexpr bool IsEngineObjectReturn =
            std::is_pointer_v<std::remove_cvref_t<R>> || IsSmartPointer<std::remove_cvref_t<R>>::value ||
            std::is_base_of_v<Component, std::remove_cvref_t<R>> ||
            std::is_same_v<std::remove_cvref_t<R>, GameObject> ||
            std::is_same_v<std::remove_cvref_t<R>, Positionable> ||
            std::is_same_v<std::remove_cvref_t<R>, Scene> ||
            std::is_same_v<std::remove_cvref_t<R>, Input::ActionMap> ||
            std::is_same_v<std::remove_cvref_t<R>, Input::InputAction>;

        // Scripts get copies of returned values: a reference into the object (e.g. a renderer's color)
        // would dangle once the object is freed
        template <typename R>
        using ForwardedReturn = std::conditional_t<std::is_lvalue_reference_v<R>, std::remove_cvref_t<R>, R>;

        template <typename R, auto Method, typename Handle, typename... Args>
        ForwardedReturn<R> CallPinned(const Handle &handle, Args &&... args)
        {
            static_assert(!IsEngineObjectReturn<R>,
                          "an engine object returned to Lua would dangle; bind this method by hand, returning a handle");
            const auto pinned = handle.Pin(); // throws if the object is gone; keeps it alive for the call
            return ((*pinned).*Method)(std::forward<Args>(args)...);
        }

        template <typename Handle, auto Method, typename Signature = decltype(Method)>
        struct Forwarder;

        template <typename Handle, auto Method, typename C, typename R, typename... A>
        struct Forwarder<Handle, Method, R (C::*)(A...)>
        {
            static ForwardedReturn<R> Call(const Handle &handle, A... args)
            {
                return CallPinned<R, Method>(handle, std::forward<A>(args)...);
            }
        };

        template <typename Handle, auto Method, typename C, typename R, typename... A>
        struct Forwarder<Handle, Method, R (C::*)(A...) const>
        {
            static ForwardedReturn<R> Call(const Handle &handle, A... args)
            {
                return CallPinned<R, Method>(handle, std::forward<A>(args)...);
            }
        };

        template <typename Handle, auto Method, typename C, typename R, typename... A>
        struct Forwarder<Handle, Method, R (C::*)(A...) noexcept>
        {
            static ForwardedReturn<R> Call(const Handle &handle, A... args)
            {
                return CallPinned<R, Method>(handle, std::forward<A>(args)...);
            }
        };

        template <typename Handle, auto Method, typename C, typename R, typename... A>
        struct Forwarder<Handle, Method, R (C::*)(A...) const noexcept>
        {
            static ForwardedReturn<R> Call(const Handle &handle, A... args)
            {
                return CallPinned<R, Method>(handle, std::forward<A>(args)...);
            }
        };
    }

    /// A Lua method on a handle type that calls Method on the live object, or raises a Lua error once it's
    /// gone, e.g. Forward<ComponentRef<Rigidbody>, &Rigidbody::SetMass>()
    template <typename Handle, auto Method>
    constexpr auto Forward()
    {
        return &Detail::Forwarder<Handle, Method>::Call;
    }
}
