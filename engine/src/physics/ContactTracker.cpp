#include "engine/physics/ContactTracker.hpp"

#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/physics/ICollider.hpp"
#include "engine/physics/Rigidbody.hpp"

#include <algorithm>
#include <ranges>

#ifdef N2ENGINE_PHYSX_ENABLED
namespace physx
{
    class PxShape;
}
#endif

namespace N2Engine::Physics
{
    template <typename ShapeKey>
    bool ContactTracker<ShapeKey>::AddTouch(TouchMap &touches, const CollisionPair &pair, ShapeKey shapeOnA,
                                            ShapeKey shapeOnB)
    {
        auto [it, inserted] = touches.try_emplace(pair);
        // The key may have been stored the other way round; keep shapes oriented to the key
        if (it->first.bodyA != pair.bodyA)
        {
            std::swap(shapeOnA, shapeOnB);
        }
        auto &shapes = it->second;
        if (std::ranges::none_of(shapes, [&](const ShapePair &p) { return p.onA == shapeOnA && p.onB == shapeOnB; }))
        {
            shapes.push_back({shapeOnA, shapeOnB});
        }
        return shapes.size() == 1 && inserted;
    }

    template <typename ShapeKey>
    bool ContactTracker<ShapeKey>::RemoveTouch(TouchMap &touches, const CollisionPair &pair, ShapeKey shapeOnA,
                                               ShapeKey shapeOnB)
    {
        const auto it = touches.find(pair);
        if (it == touches.end())
        {
            return false; // already forgotten (body destroyed or shape removed)
        }
        if (it->first.bodyA != pair.bodyA)
        {
            std::swap(shapeOnA, shapeOnB);
        }
        std::erase_if(it->second, [&](const ShapePair &p) { return p.onA == shapeOnA && p.onB == shapeOnB; });
        if (!it->second.empty())
        {
            return false; // other shapes of these bodies are still touching
        }
        touches.erase(it);
        return true;
    }

    // ===== Reports =====

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::ContactFound(const CollisionPair &pair, ShapeKey onA, ShapeKey onB,
                                                Collision *data)
    {
        if (AddTouch(collisionTouches, pair, onA, onB))
        {
            newCollisions.push_back({pair, data});
        }
        else
        {
            delete data; // another shape pair of these bodies was touching already
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::ContactLost(const CollisionPair &pair, ShapeKey onA, ShapeKey onB,
                                               Collision *data)
    {
        if (RemoveTouch(collisionTouches, pair, onA, onB))
        {
            endedCollisions.push_back({pair, data});
        }
        else
        {
            delete data; // other shape pairs of these bodies are still touching
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::TriggerFound(const CollisionPair &pair, ShapeKey onA, ShapeKey onB)
    {
        if (AddTouch(triggerTouches, pair, onA, onB))
        {
            newTriggers.push_back({pair, ColliderOf(onA), ColliderOf(onB)});
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::TriggerLost(const CollisionPair &pair, ShapeKey onA, ShapeKey onB)
    {
        if (RemoveTouch(triggerTouches, pair, onA, onB))
        {
            endedTriggers.push_back({pair, ColliderOf(onA), ColliderOf(onB)});
        }
    }

    // ===== Ending touches =====

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::EndShape(ShapeKey shape)
    {
        // A body pair whose last touching shape pair goes ends now: the engine's touch-lost for the removed shape is
        // skipped, so the other body would otherwise never get Exit, and a re-created shape would fire a second Enter.
        const auto involves = [shape](const ShapePair &p) { return p.onA == shape || p.onB == shape; };
        for (auto it = collisionTouches.begin(); it != collisionTouches.end();)
        {
            const auto removed = std::ranges::find_if(it->second, involves);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair last = *removed;
            std::erase_if(it->second, involves);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            auto *data = new Collision();
            data->collider = ColliderOf(last.onA); // LiveCollider nulls the removed one at dispatch
            data->otherCollider = ColliderOf(last.onB);
            // Owners too: a collider-only object's body is destroyed right after its shapes are removed
            forgottenCollisions.push_back({it->first, data, OwnersOf(it->first)});
            it = collisionTouches.erase(it);
        }
        for (auto it = triggerTouches.begin(); it != triggerTouches.end();)
        {
            const auto removed = std::ranges::find_if(it->second, involves);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair last = *removed;
            std::erase_if(it->second, involves);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            forgottenTriggers.push_back({it->first, ColliderOf(last.onA), ColliderOf(last.onB), OwnersOf(it->first)});
            it = triggerTouches.erase(it);
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::ForgetPairsWithBody(const PhysicsBodyHandle handle, const bool shapesReleased)
    {
        const auto involves = [handle](const CollisionPair &pair)
        { return pair.bodyA == handle || pair.bodyB == handle; };
        // Both sides get their Exit: the engine's touch-lost for a released or removed body is skipped by the
        // callbacks, so nothing else would report it. If this body is being destroyed, its side is reached through its
        // recorded GameObject (when that survives, e.g. the body is being re-created), and its shapes are already
        // released, so only the other side's collider is looked up.
        const auto colliderOn = [this, handle, shapesReleased](const CollisionPair &pair, const ShapePair &shapes,
                                                              const bool sideA)
        {
            const bool thisIsA = pair.bodyA == handle;
            return !shapesReleased || sideA != thisIsA ? ColliderOf(sideA ? shapes.onA : shapes.onB) : nullptr;
        };
        for (auto it = collisionTouches.begin(); it != collisionTouches.end();)
        {
            if (!involves(it->first) || it->second.empty())
            {
                it = involves(it->first) ? collisionTouches.erase(it) : std::next(it);
                continue;
            }
            auto *data = new Collision();
            data->collider = colliderOn(it->first, it->second.front(), true);
            data->otherCollider = colliderOn(it->first, it->second.front(), false);
            forgottenCollisions.push_back({it->first, data, OwnersOf(it->first)});
            it = collisionTouches.erase(it);
        }
        for (auto it = triggerTouches.begin(); it != triggerTouches.end();)
        {
            if (!involves(it->first) || it->second.empty())
            {
                it = involves(it->first) ? triggerTouches.erase(it) : std::next(it);
                continue;
            }
            forgottenTriggers.push_back({it->first, colliderOn(it->first, it->second.front(), true),
                                         colliderOn(it->first, it->second.front(), false), OwnersOf(it->first)});
            it = triggerTouches.erase(it);
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::EndTouches(
        const std::function<bool(const CollisionPair &, const ShapePair &)> &predicate)
    {
        for (auto it = collisionTouches.begin(); it != collisionTouches.end();)
        {
            const CollisionPair &key = it->first;
            const auto selected = [&](const ShapePair &shapes) { return predicate(key, shapes); };
            const auto removed = std::ranges::find_if(it->second, selected);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair lastPair = *removed;
            std::erase_if(it->second, selected);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            auto *data = new Collision();
            data->collider = ColliderOf(lastPair.onA);
            data->otherCollider = ColliderOf(lastPair.onB);
            endedCollisions.push_back({it->first, data});
            it = collisionTouches.erase(it);
        }
        for (auto it = triggerTouches.begin(); it != triggerTouches.end();)
        {
            const CollisionPair &key = it->first;
            const auto selected = [&](const ShapePair &shapes) { return predicate(key, shapes); };
            const auto removed = std::ranges::find_if(it->second, selected);
            if (removed == it->second.end())
            {
                ++it;
                continue;
            }
            const ShapePair lastPair = *removed;
            std::erase_if(it->second, selected);
            if (!it->second.empty())
            {
                ++it;
                continue;
            }
            endedTriggers.push_back({it->first, ColliderOf(lastPair.onA), ColliderOf(lastPair.onB)});
            it = triggerTouches.erase(it);
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::Clear()
    {
        for (auto *queue : {&newCollisions, &endedCollisions, &forgottenCollisions})
        {
            for (const auto &event : *queue)
            {
                delete event.data;
            }
            queue->clear();
        }
        newTriggers.clear();
        endedTriggers.clear();
        forgottenTriggers.clear();
        collisionTouches.clear();
        triggerTouches.clear();
    }

    // ===== Lookups =====

    template <typename ShapeKey>
    ICollider *ContactTracker<ShapeKey>::ColliderOf(ShapeKey shape) const
    {
        return _colliders ? _colliders(shape) : nullptr;
    }

    template <typename ShapeKey>
    GameObject *ContactTracker<ShapeKey>::GetBodyOwner(const PhysicsBodyHandle handle) const
    {
        BodyView view;
        if (!_bodies || !_bodies(handle, view))
        {
            return nullptr;
        }
        if (view.rigidbody)
        {
            return &view.rigidbody->GetGameObject();
        }
        if (view.colliders && !view.colliders->empty())
        {
            return &view.colliders->front()->GetGameObject();
        }
        return nullptr;
    }

    template <typename ShapeKey>
    ICollider *ContactTracker<ShapeKey>::LiveCollider(const PhysicsBodyHandle body, ICollider *collider) const
    {
        BodyView view;
        return _bodies && _bodies(body, view) && collider && view.colliders &&
                       std::ranges::find(*view.colliders, collider) != view.colliders->end()
                   ? collider
                   : nullptr;
    }

    template <typename ShapeKey>
    PairOwners ContactTracker<ShapeKey>::OwnersOf(const CollisionPair &pair) const
    {
        const auto ownerOf = [this](const PhysicsBodyHandle handle) -> std::weak_ptr<GameObject>
        {
            GameObject *owner = GetBodyOwner(handle);
            return owner ? owner->weak_from_this() : std::weak_ptr<GameObject>{};
        };
        return {ownerOf(pair.bodyA), ownerOf(pair.bodyB)};
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::DescribeSide(const PhysicsBodyHandle handle,
                                                const std::weak_ptr<GameObject> &fallbackOwner,
                                                GameObject *&gameObject, Rigidbody *&rigidbody) const
    {
        if (BodyView view; _bodies && _bodies(handle, view))
        {
            if (view.rigidbody)
            {
                gameObject = &view.rigidbody->GetGameObject();
                rigidbody = view.rigidbody;
            }
            else if (view.colliders && !view.colliders->empty())
            {
                gameObject = &view.colliders->front()->GetGameObject();
                rigidbody = nullptr;
            }
            return;
        }

        // The body is gone (destroyed or re-created); the object it belonged to may still be around
        const auto owner = fallbackOwner.lock();
        gameObject = owner && !owner->IsDestroyed() ? owner.get() : nullptr;
        rigidbody = nullptr;
    }

    // Helper to create collision data with resolved object references
    template <typename ShapeKey>
    Collision ContactTracker<ShapeKey>::CreateCollisionData(const CollisionPair &pair, const Collision &baseData,
                                                            const bool isForBodyA, const PairOwners &owners) const
    {
        Collision collision = baseData;

        DescribeSide(isForBodyA ? pair.bodyA : pair.bodyB, isForBodyA ? owners.a : owners.b, collision.gameObject,
                     collision.rigidbody);
        DescribeSide(isForBodyA ? pair.bodyB : pair.bodyA, isForBodyA ? owners.b : owners.a,
                     collision.otherGameObject, collision.otherRigidbody);

        // baseData's colliders are A's and B's; validate them, then swap for B's view
        collision.collider = LiveCollider(pair.bodyA, baseData.collider);
        collision.otherCollider = LiveCollider(pair.bodyB, baseData.otherCollider);

        // Everything directional is from A's point of view; B sees it the other way round
        if (!isForBodyA)
        {
            std::swap(collision.collider, collision.otherCollider);
            for (auto &contact : collision.contacts)
            {
                contact.normal = contact.normal * -1.0f;
            }
            collision.impulse = collision.impulse * -1.0f;
            collision.relativeVelocity = collision.relativeVelocity * -1.0f;
        }

        return collision;
    }

    template <typename ShapeKey>
    Trigger ContactTracker<ShapeKey>::CreateTriggerData(const CollisionPair &pair, ICollider *colliderA,
                                                        ICollider *colliderB, const bool isForBodyA,
                                                        const PairOwners &owners) const
    {
        Trigger trigger;
        trigger.collider = LiveCollider(isForBodyA ? pair.bodyA : pair.bodyB, isForBodyA ? colliderA : colliderB);
        trigger.otherCollider = LiveCollider(isForBodyA ? pair.bodyB : pair.bodyA, isForBodyA ? colliderB : colliderA);

        DescribeSide(isForBodyA ? pair.bodyA : pair.bodyB, isForBodyA ? owners.a : owners.b, trigger.gameObject,
                     trigger.rigidbody);
        DescribeSide(isForBodyA ? pair.bodyB : pair.bodyA, isForBodyA ? owners.b : owners.a, trigger.otherGameObject,
                     trigger.otherRigidbody);

        return trigger;
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::DispatchToBody(const PhysicsBodyHandle handle,
                                                  const std::function<void(Component &)> &fn,
                                                  const std::weak_ptr<GameObject> &fallbackOwner) const
    {
        GameObject *owner = GetBodyOwner(handle);
        const bool viaBody = owner != nullptr;
        std::shared_ptr<GameObject> keepAlive;
        if (!viaBody)
        {
            // The body is gone. Its GameObject still hears about a forgotten pair unless it's being destroyed.
            keepAlive = fallbackOwner.lock();
            if (!keepAlive || keepAlive->IsDestroyed())
            {
                return; // destroyed this frame, or never belonged to a GameObject
            }
            owner = keepAlive.get();
        }

        // Snapshot: a handler can add or remove components, which would invalidate the live vector
        std::vector<Component *> components;
        for (const auto &component : owner->GetAllComponents())
        {
            components.push_back(component.get());
        }

        for (Component *component : components)
        {
            // A handler may have destroyed this body (e.g. removed its Rigidbody) or other components
            if (viaBody ? GetBodyOwner(handle) != owner : owner->IsDestroyed())
            {
                return;
            }
            const auto &live = owner->GetAllComponents();
            if (std::ranges::none_of(live, [component](const auto &c) { return c.get() == component; }))
            {
                continue;
            }
            // Like Unity: an inactive object gets nothing (its body has left the simulation, and it got OnDisable
            // instead), but a disabled component on an active object still does, so a script that disables itself
            // keeps a balanced Enter/Exit count
            if (component->IsDestroyed() || !owner->IsActiveInHierarchy())
            {
                continue;
            }
            fn(*component);
        }
    }

    // ===== Delivery =====

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::DispatchCollisionExits(std::vector<CollisionEvent> &queue)
    {
        std::vector<CollisionEvent> events;
        events.swap(queue);
        for (const auto &event : events)
        {
            const Collision forA = CreateCollisionData(event.pair, *event.data, true, event.owners);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnCollisionExit(forA); }, event.owners.a);
            const Collision forB = CreateCollisionData(event.pair, *event.data, false, event.owners);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnCollisionExit(forB); }, event.owners.b);
            delete event.data;
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::DispatchTriggerExits(std::vector<TriggerEvent> &queue)
    {
        std::vector<TriggerEvent> events;
        events.swap(queue);
        for (const auto &event : events)
        {
            const Trigger forA = CreateTriggerData(event.pair, event.colliderA, event.colliderB, true, event.owners);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnTriggerExit(forA); }, event.owners.a);
            const Trigger forB = CreateTriggerData(event.pair, event.colliderA, event.colliderB, false, event.owners);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnTriggerExit(forB); }, event.owners.b);
        }
    }

    template <typename ShapeKey>
    void ContactTracker<ShapeKey>::Dispatch()
    {
        // Every event goes to *each* body's own GameObject (A's components get A's view, B's get B's).
        // Take the event lists first so handlers can't disturb what's being iterated.

        // ===== Exits for pairs ended by removing a body or collider =====
        // First, so a collider re-created in the same step reports Exit(old) before Enter(new)
        DispatchCollisionExits(forgottenCollisions);
        DispatchTriggerExits(forgottenTriggers);

        // ===== OnCollisionEnter =====
        std::vector<CollisionEvent> entering;
        entering.swap(newCollisions);
        for (const auto &event : entering)
        {
            const Collision forA = CreateCollisionData(event.pair, *event.data, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnCollisionEnter(forA); });
            const Collision forB = CreateCollisionData(event.pair, *event.data, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnCollisionEnter(forB); });
            delete event.data;
        }

        // ===== OnCollisionStay =====
        std::vector<CollisionPair> activeCollisions;
        for (const auto &key : collisionTouches | std::views::keys)
        {
            activeCollisions.push_back(key);
        }
        for (const auto &pair : activeCollisions)
        {
            const auto touching = collisionTouches.find(pair);
            if (touching == collisionTouches.end() || touching->second.empty())
            {
                continue; // a handler destroyed one of the bodies or removed the touching collider
            }
            Collision baseData{};
            baseData.collider = ColliderOf(touching->second.front().onA);
            baseData.otherCollider = ColliderOf(touching->second.front().onB);
            const Collision forA = CreateCollisionData(pair, baseData, true);
            DispatchToBody(pair.bodyA, [&forA](Component &c) { c.OnCollisionStay(forA); });
            const Collision forB = CreateCollisionData(pair, baseData, false);
            DispatchToBody(pair.bodyB, [&forB](Component &c) { c.OnCollisionStay(forB); });
        }

        // ===== OnCollisionExit =====
        DispatchCollisionExits(endedCollisions);

        // ===== OnTriggerEnter =====
        std::vector<TriggerEvent> enteringTriggers;
        enteringTriggers.swap(newTriggers);
        for (const auto &event : enteringTriggers)
        {
            const Trigger forA = CreateTriggerData(event.pair, event.colliderA, event.colliderB, true);
            DispatchToBody(event.pair.bodyA, [&forA](Component &c) { c.OnTriggerEnter(forA); });
            const Trigger forB = CreateTriggerData(event.pair, event.colliderA, event.colliderB, false);
            DispatchToBody(event.pair.bodyB, [&forB](Component &c) { c.OnTriggerEnter(forB); });
        }

        // ===== OnTriggerStay =====
        std::vector<CollisionPair> activeTriggers;
        for (const auto &key : triggerTouches | std::views::keys)
        {
            activeTriggers.push_back(key);
        }
        for (const auto &pair : activeTriggers)
        {
            const auto touching = triggerTouches.find(pair);
            if (touching == triggerTouches.end() || touching->second.empty())
            {
                continue;
            }
            ICollider *colliderA = ColliderOf(touching->second.front().onA);
            ICollider *colliderB = ColliderOf(touching->second.front().onB);
            const Trigger forA = CreateTriggerData(pair, colliderA, colliderB, true);
            DispatchToBody(pair.bodyA, [&forA](Component &c) { c.OnTriggerStay(forA); });
            const Trigger forB = CreateTriggerData(pair, colliderA, colliderB, false);
            DispatchToBody(pair.bodyB, [&forB](Component &c) { c.OnTriggerStay(forB); });
        }

        // ===== OnTriggerExit =====
        DispatchTriggerExits(endedTriggers);
    }

    // The two instantiations: the PhysX backend keys its touches by shape, the Jolt backend by collider
#ifdef N2ENGINE_PHYSX_ENABLED
    template class ContactTracker<const physx::PxShape *>;
#endif
    template class ContactTracker<const ICollider *>;
}
