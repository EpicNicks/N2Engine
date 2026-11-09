#include "ContactTracker2D.hpp"

#include <algorithm>
#include <utility>

#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"

namespace N2Engine::Physics2D
{
    void ContactTracker2D::Normalise(const PhysicsBodyHandle bodyX, const ShapeKey shapeX, const PhysicsBodyHandle bodyY,
                                     const ShapeKey shapeY, BodyPair &pair, ShapePair &shapes)
    {
        if (HandleLess{}(bodyY, bodyX))
        {
            pair = {bodyY, bodyX};
            shapes = {shapeY, shapeX};
        }
        else
        {
            pair = {bodyX, bodyY};
            shapes = {shapeX, shapeY};
        }
    }

    bool ContactTracker2D::AddTouch(TouchMap &touches, const BodyPair &pair, const ShapePair &shapes)
    {
        auto &list = touches[pair];
        const bool first = list.empty();
        const bool known = std::ranges::any_of(list, [&](const ShapePair &s) { return s.onA == shapes.onA && s.onB == shapes.onB; });
        if (!known)
        {
            list.push_back(shapes);
        }
        return first;
    }

    bool ContactTracker2D::RemoveTouch(TouchMap &touches, const BodyPair &pair, const ShapePair &shapes)
    {
        const auto it = touches.find(pair);
        if (it == touches.end())
        {
            return false; // already ended by removing a shape or body
        }
        const auto removed = std::erase_if(it->second, [&](const ShapePair &s) { return s.onA == shapes.onA && s.onB == shapes.onB; });
        if (removed == 0 || !it->second.empty())
        {
            return false;
        }
        touches.erase(it);
        return true;
    }

    void ContactTracker2D::CollisionBegan(const PhysicsBodyHandle bodyX, const ShapeKey shapeX, const PhysicsBodyHandle bodyY, const ShapeKey shapeY)
    {
        Event event;
        Normalise(bodyX, shapeX, bodyY, shapeY, event.pair, event.shapes);
        if (AddTouch(_collisions, event.pair, event.shapes))
        {
            _newCollisions.push_back(event);
        }
    }

    void ContactTracker2D::CollisionEnded(const PhysicsBodyHandle bodyX, const ShapeKey shapeX, const PhysicsBodyHandle bodyY, const ShapeKey shapeY)
    {
        Event event;
        Normalise(bodyX, shapeX, bodyY, shapeY, event.pair, event.shapes);
        if (RemoveTouch(_collisions, event.pair, event.shapes))
        {
            _endedCollisions.push_back(event);
        }
    }

    void ContactTracker2D::TriggerBegan(const PhysicsBodyHandle bodyX, const ShapeKey shapeX, const PhysicsBodyHandle bodyY, const ShapeKey shapeY)
    {
        Event event;
        Normalise(bodyX, shapeX, bodyY, shapeY, event.pair, event.shapes);
        if (AddTouch(_triggers, event.pair, event.shapes))
        {
            _newTriggers.push_back(event);
        }
    }

    void ContactTracker2D::TriggerEnded(const PhysicsBodyHandle bodyX, const ShapeKey shapeX, const PhysicsBodyHandle bodyY, const ShapeKey shapeY)
    {
        Event event;
        Normalise(bodyX, shapeX, bodyY, shapeY, event.pair, event.shapes);
        if (RemoveTouch(_triggers, event.pair, event.shapes))
        {
            _endedTriggers.push_back(event);
        }
    }

    ContactTracker2D::Owners ContactTracker2D::OwnersOf(const BodyPair &pair) const
    {
        const auto ownerOf = [this](const PhysicsBodyHandle body) -> std::weak_ptr<GameObject>
        {
            GameObject *gameObject = nullptr;
            Rigidbody2D *rigidbody = nullptr;
            if (_host.describeBody(body, gameObject, rigidbody) && gameObject)
            {
                return gameObject->weak_from_this();
            }
            return {};
        };
        return {ownerOf(pair.a), ownerOf(pair.b)};
    }

    void ContactTracker2D::EndWhere(TouchMap &touches, std::vector<Event> &forgotten, const std::function<bool(const BodyPair &, const ShapePair &)> &ends)
    {
        for (auto it = touches.begin(); it != touches.end();)
        {
            ShapePair last;
            const auto removed = std::erase_if(it->second, [&](const ShapePair &s)
            {
                if (!ends(it->first, s))
                {
                    return false;
                }
                last = s;
                return true;
            });
            if (removed == 0 || !it->second.empty())
            {
                ++it;
                continue;
            }
            forgotten.push_back({it->first, last, OwnersOf(it->first)});
            it = touches.erase(it);
        }
    }

    void ContactTracker2D::EndShape(const ShapeKey shape)
    {
        const auto involves = [shape](const BodyPair &, const ShapePair &s) { return s.onA == shape || s.onB == shape; };
        EndWhere(_collisions, _forgottenCollisions, involves);
        EndWhere(_triggers, _forgottenTriggers, involves);
    }

    void ContactTracker2D::EndBody(const PhysicsBodyHandle body)
    {
        const auto involves = [body](const BodyPair &pair, const ShapePair &) { return pair.a == body || pair.b == body; };
        EndWhere(_collisions, _forgottenCollisions, involves);
        EndWhere(_triggers, _forgottenTriggers, involves);
    }

    void ContactTracker2D::DescribeSide(const PhysicsBodyHandle body, const std::weak_ptr<GameObject> &fallback, GameObject *&gameObject, Rigidbody2D *&rigidbody) const
    {
        gameObject = nullptr;
        rigidbody = nullptr;
        if (!_host.describeBody(body, gameObject, rigidbody))
        {
            gameObject = fallback.lock().get(); // kept alive by the delivery's own locks, or null
        }
    }

    void ContactTracker2D::DeliverToBody(const PhysicsBodyHandle body, const std::weak_ptr<GameObject> &fallback, const std::function<void(Component &)> &fn) const
    {
        GameObject *owner = nullptr;
        Rigidbody2D *rigidbody = nullptr;
        const bool viaBody = _host.describeBody(body, owner, rigidbody) && owner;
        std::shared_ptr<GameObject> keepAlive;
        if (!viaBody)
        {
            // The body is gone. Its GameObject still hears about the pair, unless it is being destroyed.
            keepAlive = fallback.lock();
            if (!keepAlive || keepAlive->IsDestroyed())
            {
                return;
            }
            owner = keepAlive.get();
        }

        // A snapshot: a handler can add or remove components
        std::vector<Component *> components;
        for (const auto &component : owner->GetAllComponents())
        {
            components.push_back(component.get());
        }
        for (Component *component : components)
        {
            // A handler may have destroyed this body or the other components. Every check here is against live state.
            if (viaBody)
            {
                GameObject *currentOwner = nullptr;
                if (!_host.describeBody(body, currentOwner, rigidbody) || currentOwner != owner)
                {
                    return;
                }
            }
            else if (owner->IsDestroyed())
            {
                return;
            }
            const auto &live = owner->GetAllComponents();
            if (std::ranges::none_of(live, [component](const auto &c) { return c.get() == component; }))
            {
                continue;
            }
            // As in 3D: an inactive object gets nothing, a disabled component on an active object still does
            if (component->IsDestroyed() || !owner->IsActiveInHierarchy())
            {
                continue;
            }
            fn(*component);
        }
    }

    void ContactTracker2D::DeliverCollision(const Event &event, void (Component::*handler)(const Collision2D &), const ContactData &data)
    {
        // A side whose body is gone is described through these, so they must outlive the delivery
        const auto keepA = event.owners.a.lock();
        const auto keepB = event.owners.b.lock();
        const auto forReceiver = [&](const bool receiverIsA)
        {
            Collision2D collision;
            GameObject *gameObjectA = nullptr, *gameObjectB = nullptr;
            Rigidbody2D *rigidbodyA = nullptr, *rigidbodyB = nullptr;
            DescribeSide(event.pair.a, event.owners.a, gameObjectA, rigidbodyA);
            DescribeSide(event.pair.b, event.owners.b, gameObjectB, rigidbodyB);
            Collider2D *colliderA = _host.colliderOf(event.shapes.onA);
            Collider2D *colliderB = _host.colliderOf(event.shapes.onB);
            collision.gameObject = receiverIsA ? gameObjectA : gameObjectB;
            collision.rigidbody = receiverIsA ? rigidbodyA : rigidbodyB;
            collision.otherGameObject = receiverIsA ? gameObjectB : gameObjectA;
            collision.otherRigidbody = receiverIsA ? rigidbodyB : rigidbodyA;
            collision.collider = receiverIsA ? colliderA : colliderB;
            collision.otherCollider = receiverIsA ? colliderB : colliderA;
            // Stored normals point from a to b, which is "from the other to this" for b
            const float sign = receiverIsA ? -1.0f : 1.0f;
            collision.contacts = data.contacts;
            for (ContactPoint2D &point : collision.contacts)
            {
                point.normal = Math::Vector2(point.normal.x * sign, point.normal.y * sign);
            }
            collision.relativeVelocity = Math::Vector2(data.velocityAMinusB.x * -sign, data.velocityAMinusB.y * -sign);
            collision.impulse = Math::Vector2(data.impulseOnB.x * sign, data.impulseOnB.y * sign);
            return collision;
        };
        const Collision2D forA = forReceiver(true);
        DeliverToBody(event.pair.a, event.owners.a, [&](Component &c) { (c.*handler)(forA); });
        const Collision2D forB = forReceiver(false);
        DeliverToBody(event.pair.b, event.owners.b, [&](Component &c) { (c.*handler)(forB); });
    }

    void ContactTracker2D::DeliverTrigger(const Event &event, void (Component::*handler)(const Trigger2D &))
    {
        // A side whose body is gone is described through these, so they must outlive the delivery
        const auto keepA = event.owners.a.lock();
        const auto keepB = event.owners.b.lock();
        const auto forReceiver = [&](const bool receiverIsA)
        {
            Trigger2D trigger;
            GameObject *gameObjectA = nullptr, *gameObjectB = nullptr;
            Rigidbody2D *rigidbodyA = nullptr, *rigidbodyB = nullptr;
            DescribeSide(event.pair.a, event.owners.a, gameObjectA, rigidbodyA);
            DescribeSide(event.pair.b, event.owners.b, gameObjectB, rigidbodyB);
            Collider2D *colliderA = _host.colliderOf(event.shapes.onA);
            Collider2D *colliderB = _host.colliderOf(event.shapes.onB);
            trigger.gameObject = receiverIsA ? gameObjectA : gameObjectB;
            trigger.rigidbody = receiverIsA ? rigidbodyA : rigidbodyB;
            trigger.otherGameObject = receiverIsA ? gameObjectB : gameObjectA;
            trigger.otherRigidbody = receiverIsA ? rigidbodyB : rigidbodyA;
            trigger.collider = receiverIsA ? colliderA : colliderB;
            trigger.otherCollider = receiverIsA ? colliderB : colliderA;
            return trigger;
        };
        const Trigger2D forA = forReceiver(true);
        DeliverToBody(event.pair.a, event.owners.a, [&](Component &c) { (c.*handler)(forA); });
        const Trigger2D forB = forReceiver(false);
        DeliverToBody(event.pair.b, event.owners.b, [&](Component &c) { (c.*handler)(forB); });
    }

    void ContactTracker2D::Dispatch()
    {
        // Each list is taken first, so a handler that ends a pair queues it for the next dispatch instead of disturbing this one
        const auto take = [](std::vector<Event> &queue)
        {
            std::vector<Event> taken;
            taken.swap(queue);
            return taken;
        };
        const auto stayEvents = [](const TouchMap &touches)
        {
            std::vector<Event> events;
            for (const auto &[pair, shapes] : touches)
            {
                if (!shapes.empty())
                {
                    events.push_back({pair, shapes.front(), {}});
                }
            }
            return events;
        };
        const auto stillTouching = [](const TouchMap &touches, const Event &event)
        {
            const auto it = touches.find(event.pair);
            return it != touches.end() && !it->second.empty();
        };
        const auto readContacts = [this](const Event &event)
        {
            ContactData data;
            if (_host.colliderOf(event.shapes.onA) && _host.colliderOf(event.shapes.onB))
            {
                _host.readContacts(event.pair, event.shapes, data);
            }
            return data;
        };

        // First, so a collider re-created in the same step reports Exit(old) before Enter(new)
        for (const Event &event : take(_forgottenCollisions))
        {
            DeliverCollision(event, &Component::OnCollisionExit2D, {});
        }
        for (const Event &event : take(_forgottenTriggers))
        {
            DeliverTrigger(event, &Component::OnTriggerExit2D);
        }

        for (const Event &event : take(_newCollisions))
        {
            if (stillTouching(_collisions, event))
            {
                DeliverCollision(event, &Component::OnCollisionEnter2D, readContacts(event));
            }
        }
        for (const Event &event : stayEvents(_collisions))
        {
            if (stillTouching(_collisions, event))
            {
                DeliverCollision(event, &Component::OnCollisionStay2D, readContacts(event));
            }
        }
        for (const Event &event : take(_endedCollisions))
        {
            DeliverCollision(event, &Component::OnCollisionExit2D, {});
        }

        for (const Event &event : take(_newTriggers))
        {
            if (stillTouching(_triggers, event))
            {
                DeliverTrigger(event, &Component::OnTriggerEnter2D);
            }
        }
        for (const Event &event : stayEvents(_triggers))
        {
            if (stillTouching(_triggers, event))
            {
                DeliverTrigger(event, &Component::OnTriggerStay2D);
            }
        }
        for (const Event &event : take(_endedTriggers))
        {
            DeliverTrigger(event, &Component::OnTriggerExit2D);
        }
    }

    void ContactTracker2D::Clear()
    {
        _collisions.clear();
        _triggers.clear();
        _forgottenCollisions.clear();
        _forgottenTriggers.clear();
        _newCollisions.clear();
        _endedCollisions.clear();
        _newTriggers.clear();
        _endedTriggers.clear();
    }
}
