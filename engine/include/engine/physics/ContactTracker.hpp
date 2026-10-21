#pragma once

// The touch bookkeeping and event delivery that both physics backends share (stage J4 of the physics plan; it was
// part of PhysXBackend before). A backend reports what its engine says about shapes touching (found, lost) and the
// tracker turns that into OnCollisionEnter/Stay/Exit and OnTriggerEnter/Stay/Exit on the bodies' GameObjects, per BODY
// pair: Enter on the first touching shape pair, Exit on the last, Stay for every pair that touches when events are
// dispatched. Nothing here knows PhysX or Jolt: a "shape" is whatever key the backend picks (a PxShape pointer, a
// collider pointer), and the backend gives the tracker two lookups (the body behind a handle, the collider behind a
// shape key).
//
// Not thread safe, main thread only: Jolt's contact listener runs on worker threads and so buffers its reports first
// (JoltBackend.cpp); PhysX's callbacks run inside fetchResults on the calling thread.

#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsTypes.hpp"

namespace N2Engine
{
    class Component;
    class GameObject;
}

namespace N2Engine::Physics
{
    class ICollider;
    class Rigidbody;

    struct CollisionPair
    {
        PhysicsBodyHandle bodyA;
        PhysicsBodyHandle bodyB;

        bool operator==(const CollisionPair &other) const
        {
            return (bodyA == other.bodyA && bodyB == other.bodyB) || (bodyA == other.bodyB && bodyB == other.bodyA);
        }
    };

    // Orders pairs by their bodies' handles, smaller side first (A,B and B,A are the same key, like operator==).
    // Handle indices come from a deterministic free list, so iteration order depends only on the sequence of body
    // creations and destructions, never on addresses or the standard library's hash. (Slot reuse means it follows the
    // slot, not strictly the age, of a body.)
    struct CollisionPairLess
    {
        bool operator()(const CollisionPair &a, const CollisionPair &b) const
        {
            const auto key = [](const CollisionPair &pair)
            {
                const auto lessHandle = [](const PhysicsBodyHandle &x, const PhysicsBodyHandle &y)
                { return x.index != y.index ? x.index < y.index : x.generation < y.generation; };
                return lessHandle(pair.bodyB, pair.bodyA) ? std::pair{pair.bodyB, pair.bodyA}
                                                          : std::pair{pair.bodyA, pair.bodyB};
            };
            const auto [a0, a1] = key(a);
            const auto [b0, b1] = key(b);
            if (a0.index != b0.index)
                return a0.index < b0.index;
            if (a0.generation != b0.generation)
                return a0.generation < b0.generation;
            if (a1.index != b1.index)
                return a1.index < b1.index;
            return a1.generation < b1.generation;
        }
    };

    // The GameObjects that owned each body when a pair was ended by removing a body or collider. A side whose body is
    // gone by dispatch (destroyed, or re-created by SetBodyType / adding or removing a Rigidbody) still gets its Exit
    // through its GameObject, unless that is being destroyed. With the re-created body's later Enter, Enter and Exit
    // stay balanced on both sides.
    struct PairOwners
    {
        std::weak_ptr<GameObject> a;
        std::weak_ptr<GameObject> b;
    };

    template <typename ShapeKey>
    class ContactTracker
    {
    public:
        // Events are per body pair, but backends report touches per shape pair. Each body pair maps to the shape pairs
        // currently touching (oriented to the key's bodyA/bodyB): Enter fires when the first starts touching and Exit
        // when the last stops, so a compound body touching through two shapes does not exit while one is still in
        // contact.
        struct ShapePair
        {
            ShapeKey onA{};
            ShapeKey onB{};
        };
        // std::map, not unordered_map: the touch maps are iterated to emit Stay events, to wake partners of re-filtered
        // shapes and to emit the Exit events of removed bodies and shapes, and that order must not depend on hashing.
        using TouchMap = std::map<CollisionPair, std::vector<ShapePair>, CollisionPairLess>;

        struct CollisionEvent
        {
            CollisionPair pair;
            Collision *data = nullptr; // collider/otherCollider are pair.bodyA's and pair.bodyB's; owned by the event
            PairOwners owners;         // only for forgotten pairs
        };

        struct TriggerEvent
        {
            CollisionPair pair;
            ICollider *colliderA = nullptr; // on pair.bodyA
            ICollider *colliderB = nullptr; // on pair.bodyB
            PairOwners owners;              // only for forgotten pairs
        };

        /// What the tracker needs to know about a body: its Rigidbody (null for a collider-only body) and its
        /// registered colliders in registration order. Pointers stay valid until the next call into the backend.
        struct BodyView
        {
            Rigidbody *rigidbody = nullptr;
            const std::vector<ICollider *> *colliders = nullptr;
        };
        /// False for a handle that is stale or invalid
        using BodyLookup = std::function<bool(PhysicsBodyHandle, BodyView &)>;
        /// The collider a shape key belongs to (null for a null key)
        using ColliderLookup = std::function<ICollider *(ShapeKey)>;

        ContactTracker() = default;
        ContactTracker(const ContactTracker &) = delete;
        ContactTracker &operator=(const ContactTracker &) = delete;
        ~ContactTracker() { Clear(); }

        void SetHost(BodyLookup bodies, ColliderLookup colliders)
        {
            _bodies = std::move(bodies);
            _colliders = std::move(colliders);
        }

        // ===== State, public so a backend's own re-filtering code can read and settle it =====
        std::vector<CollisionEvent> newCollisions;
        TouchMap collisionTouches;
        std::vector<CollisionEvent> endedCollisions;
        std::vector<TriggerEvent> newTriggers;
        TouchMap triggerTouches;
        std::vector<TriggerEvent> endedTriggers;
        // Ended by removing a body or collider rather than reported by the physics engine; dispatched before the
        // step's Enter events
        std::vector<CollisionEvent> forgottenCollisions;
        std::vector<TriggerEvent> forgottenTriggers;

        /// @returns true if this is the body pair's first touching shape pair
        static bool AddTouch(TouchMap &touches, const CollisionPair &pair, ShapeKey shapeOnA, ShapeKey shapeOnB);
        /// @returns true if the body pair has no touching shape pairs left
        static bool RemoveTouch(TouchMap &touches, const CollisionPair &pair, ShapeKey shapeOnA, ShapeKey shapeOnB);

        // ===== Reports (what Jolt feeds in; PhysX calls AddTouch/RemoveTouch itself in its callbacks) =====
        /// A shape pair of the pair's bodies started touching. `data` (colliders on A and B, contact points) is taken:
        /// queued as the Enter event when this is the body pair's first touching shape pair, freed otherwise.
        void ContactFound(const CollisionPair &pair, ShapeKey onA, ShapeKey onB, Collision *data);
        /// A shape pair stopped touching; `data` is queued as the Exit event when it was the last one, else freed
        void ContactLost(const CollisionPair &pair, ShapeKey onA, ShapeKey onB, Collision *data);
        /// The trigger flavours (the pair's bodyA holds the trigger collider, onA is that collider's key)
        void TriggerFound(const CollisionPair &pair, ShapeKey onA, ShapeKey onB);
        void TriggerLost(const CollisionPair &pair, ShapeKey onA, ShapeKey onB);

        // ===== Ending touches the physics engine does not report =====
        /// Drops a shape that is being removed from every touch list. A body pair left with nothing queues its Exit
        /// as a forgotten pair (both shapes are still alive: the caller releases them after this).
        void EndShape(ShapeKey shape);
        /// Ends every touch of the body (it was destroyed, or taken out of the simulation), queueing the Exit of both
        /// sides as forgotten pairs. shapesReleased: the body's shapes are already freed, so only the other side's
        /// collider can be read.
        void ForgetPairsWithBody(PhysicsBodyHandle handle, bool shapesReleased);
        /// Ends every touching shape pair the predicate selects (a matrix change that rules a pair out): a body pair
        /// left with nothing queues its Exit as an ended pair (colliders only, no contact data). Visits the touch maps
        /// in their (deterministic) order, collision touches first.
        void EndTouches(const std::function<bool(const CollisionPair &, const ShapePair &)> &predicate);

        // ===== Delivery =====
        /// Dispatches this step's events to the bodies' GameObjects, in this order: forgotten pairs' Exit, collision
        /// Enter, Stay, Exit, then trigger Enter, Stay, Exit. Every event goes to each body's own GameObject.
        void Dispatch();

        /// Frees the event data still queued and forgets every touch (Shutdown)
        void Clear();

        // ===== Lookups a backend's query results use too (a raycast hit describes its body the same way) =====
        /// The collider if it is still registered on the body, else null (a handler may have removed it)
        [[nodiscard]] ICollider *LiveCollider(PhysicsBodyHandle body, ICollider *collider) const;
        /// The GameObject and Rigidbody to report for one side of a pair: the body's, or if the body is gone, the
        /// recorded owner's (with no Rigidbody)
        void DescribeSide(PhysicsBodyHandle handle, const std::weak_ptr<GameObject> &fallbackOwner,
                          GameObject *&gameObject, Rigidbody *&rigidbody) const;

    private:
        BodyLookup _bodies;
        ColliderLookup _colliders;

        [[nodiscard]] ICollider *ColliderOf(ShapeKey shape) const;
        [[nodiscard]] GameObject *GetBodyOwner(PhysicsBodyHandle handle) const;
        [[nodiscard]] PairOwners OwnersOf(const CollisionPair &pair) const;
        Collision CreateCollisionData(const CollisionPair &pair, const Collision &baseData, bool isForBodyA,
                                      const PairOwners &owners = {}) const;
        Trigger CreateTriggerData(const CollisionPair &pair, ICollider *colliderA, ICollider *colliderB,
                                  bool isForBodyA, const PairOwners &owners = {}) const;
        void DispatchToBody(PhysicsBodyHandle handle, const std::function<void(Component &)> &fn,
                            const std::weak_ptr<GameObject> &fallbackOwner = {}) const;
        void DispatchCollisionExits(std::vector<CollisionEvent> &queue);
        void DispatchTriggerExits(std::vector<TriggerEvent> &queue);
    };
}
