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
}

// What Lua holds instead of raw engine pointers. A script can keep any of these (in `self`, a global or a
// closure) past the object's lifetime: using it afterwards raises a Lua error ("attempt to use a destroyed
// Rigidbody") instead of touching freed memory, and IsValid() reports whether it's still usable.
namespace N2Engine::Scripting
{
    /// A GameObject in Lua. Doesn't keep the object alive, except for objects a script created or detached,
    /// which the script may be the only owner of. Invalid once the object is destroyed (for an object in a
    /// scene, at the end of the frame Destroy was called in) or freed.
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

        /// Keeps the object alive from now on, e.g. when a script detaches it from its only owner
        void TakeOwnership() { _owned = _object.lock(); }

        [[nodiscard]] bool IsValid() const
        {
            const GameObject::Ptr object = _object.lock();
            return object && !object->IsDestroyed();
        }

        /// The object even if it's destroyed, or null once it's freed
        [[nodiscard]] GameObject::Ptr Lock() const { return _object.lock(); }

        /// The live object, held for the duration of a call. Throws when it's destroyed; sol turns that
        /// into a Lua error at the call site.
        [[nodiscard]] GameObject::Ptr Pin() const
        {
            GameObject::Ptr object = _object.lock();
            if (!object || object->IsDestroyed())
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
        [[nodiscard]] bool IsValid() const { return !_lifetime.expired() && !_component->IsDestroyed(); }

        [[nodiscard]] bool RefersTo(const ComponentRefBase &other) const
        {
            return !_lifetime.owner_before(other._lifetime) && !other._lifetime.owner_before(_lifetime);
        }

    protected:
        explicit ComponentRefBase(const Component &component)
            : _component(&component), _lifetime(component.GetLifetimeToken())
        {
        }

        // Only dereferenced while _lifetime hasn't expired
        const Component *_component;
        std::weak_ptr<const bool> _lifetime;
    };

    /// A component in Lua, as its concrete type. Invalid once the component is destroyed (removed, its
    /// GameObject destroyed, or the scene unloaded).
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

        /// The live component. Throws when it's destroyed; sol turns that into a Lua error at the call site.
        [[nodiscard]] T *Pin() const
        {
            if (!IsValid())
            {
                throw std::runtime_error(std::format("attempt to use a destroyed {}", s_luaName));
            }
            return _typed;
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
            if (!owner || owner->IsDestroyed() || !owner->GetPositionable())
            {
                throw std::runtime_error("attempt to use the Positionable of a destroyed GameObject");
            }
            return std::shared_ptr<Positionable>(owner, owner->GetPositionable());
        }

        [[nodiscard]] bool RefersTo(const PositionableRef &other) const { return _owner.RefersTo(other._owner); }

    private:
        GameObjectRef _owner;
    };

    /// Lua __eq for a handle type: true for two handles to the same object
    template <typename Handle>
    bool SameRef(const sol::object &a, const sol::object &b)
    {
        return a.is<Handle>() && b.is<Handle>() && a.as<Handle>().RefersTo(b.as<Handle>());
    }

    namespace Detail
    {
        // Scripts get copies of returned values: a reference into the object (e.g. a renderer's color)
        // would dangle once the object is freed
        template <typename R>
        using ForwardedReturn = std::conditional_t<std::is_lvalue_reference_v<R>, std::remove_cvref_t<R>, R>;

        template <typename R, auto Method, typename Handle, typename... Args>
        ForwardedReturn<R> CallPinned(const Handle &handle, Args &&... args)
        {
            static_assert(!std::is_pointer_v<R>,
                          "a returned pointer would dangle in Lua; bind this method by hand, returning a handle");
            const auto pinned = handle.Pin(); // throws if the object is gone
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
