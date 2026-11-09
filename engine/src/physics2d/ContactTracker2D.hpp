#pragma once

// Turns reports about shape pairs into OnCollision*2D and OnTrigger*2D events per BODY pair, by the rules of the 3D
// ContactTracker (engine/physics/ContactTracker.hpp): Enter on the first touching shape pair, Exit on the last, Stay every
// dispatch while touching, including the one with the Enter. It is separate from that class because that one is written
// against the 3D Rigidbody, ICollider and Collision, and both 3D backends rely on it unchanged. Nothing here names Box2D.
// Main thread only.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "engine/physics2d/Physics2DTypes.hpp"

namespace N2Engine
{
    class Component;
}

namespace N2Engine::Physics2D
{
    using Physics::PhysicsBodyHandle;

    struct HandleLess
    {
        bool operator()(const PhysicsBodyHandle &a, const PhysicsBodyHandle &b) const
        {
            return std::tie(a.index, a.generation) < std::tie(b.index, b.generation);
        }
    };

    class ContactTracker2D
    {
    public:
        using ShapeKey = std::uint64_t;

        /// Always stored with the lower handle in `a`, so a pair has one key and the maps' order depends only on handles
        struct BodyPair
        {
            PhysicsBodyHandle a;
            PhysicsBodyHandle b;
        };
        struct BodyPairLess
        {
            bool operator()(const BodyPair &x, const BodyPair &y) const
            {
                return std::tie(x.a.index, x.a.generation, x.b.index, x.b.generation) <
                       std::tie(y.a.index, y.a.generation, y.b.index, y.b.generation);
            }
        };
        struct ShapePair
        {
            ShapeKey onA = 0;
            ShapeKey onB = 0;
        };
        /// A collision's contact data with normals pointing from body a to body b, before it is turned to each receiver
        struct ContactData
        {
            std::vector<ContactPoint2D> contacts;
            Math::Vector2 velocityAMinusB;
            Math::Vector2 impulseOnB;
        };

        struct Host
        {
            /// False when the body is gone
            std::function<bool(PhysicsBodyHandle, GameObject *&, Rigidbody2D *&)> describeBody;
            /// Null when the shape is gone
            std::function<Collider2D *(ShapeKey)> colliderOf;
            std::function<void(const BodyPair &, const ShapePair &, ContactData &)> readContacts;
        };

        explicit ContactTracker2D(Host host) : _host(std::move(host)) {}

        void CollisionBegan(PhysicsBodyHandle bodyX, ShapeKey shapeX, PhysicsBodyHandle bodyY, ShapeKey shapeY);
        void CollisionEnded(PhysicsBodyHandle bodyX, ShapeKey shapeX, PhysicsBodyHandle bodyY, ShapeKey shapeY);
        void TriggerBegan(PhysicsBodyHandle bodyX, ShapeKey shapeX, PhysicsBodyHandle bodyY, ShapeKey shapeY);
        void TriggerEnded(PhysicsBodyHandle bodyX, ShapeKey shapeX, PhysicsBodyHandle bodyY, ShapeKey shapeY);

        /// A shape is being removed, while its body still exists: its pairs end, delivered first in the next dispatch
        void EndShape(ShapeKey shape);
        /// A body is being removed or disabled, while it can still be described: every pair of it ends
        void EndBody(PhysicsBodyHandle body);

        /// Forgotten-pair Exit, collision Enter, Stay, Exit, then trigger Enter, Stay, Exit: the 3D order
        void Dispatch();
        void Clear();

    private:
        using TouchMap = std::map<BodyPair, std::vector<ShapePair>, BodyPairLess>;
        struct Owners
        {
            std::weak_ptr<GameObject> a;
            std::weak_ptr<GameObject> b;
        };
        struct Event
        {
            BodyPair pair;
            ShapePair shapes;
            Owners owners; // only for pairs ended by removing something
        };

        static void Normalise(PhysicsBodyHandle bodyX, ShapeKey shapeX, PhysicsBodyHandle bodyY, ShapeKey shapeY, BodyPair &pair,
                              ShapePair &shapes);
        static bool AddTouch(TouchMap &touches, const BodyPair &pair, const ShapePair &shapes);
        static bool RemoveTouch(TouchMap &touches, const BodyPair &pair, const ShapePair &shapes);
        [[nodiscard]] Owners OwnersOf(const BodyPair &pair) const;
        void EndWhere(TouchMap &touches, std::vector<Event> &forgotten, const std::function<bool(const BodyPair &, const ShapePair &)> &ends);

        void DeliverCollision(const Event &event, void (Component::*handler)(const Collision2D &), const ContactData &data);
        void DeliverTrigger(const Event &event, void (Component::*handler)(const Trigger2D &));
        void DeliverToBody(PhysicsBodyHandle body, const std::weak_ptr<GameObject> &fallback, const std::function<void(Component &)> &fn) const;
        void DescribeSide(PhysicsBodyHandle body, const std::weak_ptr<GameObject> &fallback, GameObject *&gameObject, Rigidbody2D *&rigidbody) const;

        Host _host;
        TouchMap _collisions;
        TouchMap _triggers;
        std::vector<Event> _forgottenCollisions;
        std::vector<Event> _forgottenTriggers;
        std::vector<Event> _newCollisions;
        std::vector<Event> _endedCollisions;
        std::vector<Event> _newTriggers;
        std::vector<Event> _endedTriggers;
    };
}
