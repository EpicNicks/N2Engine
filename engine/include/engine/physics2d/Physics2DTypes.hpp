#pragma once

// The data 2D physics hands to game code. It mirrors the 3D types in engine/physics/PhysicsTypes.hpp and Raycast.hpp: same
// field names and meanings, with 2D vectors and 2D components. Nothing here names Box2D (docs/physics2d.html).

#include <cstdint>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine
{
    class GameObject;
}

namespace N2Engine::Physics2D
{
    class Rigidbody2D;
    class Collider2D;

    enum class BodyType2D
    {
        Static,    // Doesn't move (walls)
        Dynamic,   // Moved by forces and collisions (crates, the player, projectiles)
        Kinematic  // Moved by its velocity or its transform, not by forces (moving platforms)
    };

    // Literals as values, so renaming a constant does not change what scenes save
    N2_SERIALIZE_ENUM(BodyType2D, {
                      {BodyType2D::Static, "Static"},
                      {BodyType2D::Dynamic, "Dynamic"},
                      {BodyType2D::Kinematic, "Kinematic"}
                      })

    struct ContactPoint2D
    {
        Math::Vector2 point;        // World position
        Math::Vector2 normal;       // Points from the other collider to this one
        float separation = 0.0f;    // Negative when overlapping
        float normalImpulse = 0.0f; // Along the normal
        float tangentImpulse = 0.0f; // Friction
    };

    /// What OnCollisionEnter2D, Stay2D and Exit2D receive, from the receiver's point of view. A side's Rigidbody2D is null
    /// when it has none (a collider alone gets its own static body), and a collider that was removed is null.
    struct Collision2D
    {
        GameObject *gameObject = nullptr;
        Rigidbody2D *rigidbody = nullptr;
        GameObject *otherGameObject = nullptr;
        Rigidbody2D *otherRigidbody = nullptr;
        Collider2D *collider = nullptr;
        Collider2D *otherCollider = nullptr;

        /// The points of the first touching collider pair; empty on Exit
        std::vector<ContactPoint2D> contacts;
        /// This body's velocity minus the other's, after this step's solve: it is NOT the approach speed
        Math::Vector2 relativeVelocity;
        /// The impulse this step's contact applied to this body
        Math::Vector2 impulse;

        [[nodiscard]] const ContactPoint2D *GetContact() const { return contacts.empty() ? nullptr : &contacts[0]; }
    };

    /// What OnTriggerEnter2D, Stay2D and Exit2D receive
    struct Trigger2D
    {
        GameObject *gameObject = nullptr;
        Rigidbody2D *rigidbody = nullptr;
        GameObject *otherGameObject = nullptr;
        Rigidbody2D *otherRigidbody = nullptr;
        Collider2D *collider = nullptr;
        Collider2D *otherCollider = nullptr;
    };

    struct RaycastHit2D
    {
        bool hit = false;
        Math::Vector2 point;
        Math::Vector2 normal;
        float distance = 0.0f;
        GameObject *gameObject = nullptr;
        Rigidbody2D *rigidbody = nullptr;
        Collider2D *collider = nullptr;
        Physics::PhysicsBodyHandle bodyHandle;
    };

    struct OverlapHit2D
    {
        GameObject *gameObject = nullptr;
        Rigidbody2D *rigidbody = nullptr;
        Collider2D *collider = nullptr;
        Physics::PhysicsBodyHandle bodyHandle;
    };
}
