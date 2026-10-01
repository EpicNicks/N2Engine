#pragma once

#include <math/Vector3.hpp>
#include <vector>

namespace N2Engine
{
    class GameObject;
}

namespace N2Engine::Physics
{
    // Declared here, not in N2Engine: a stray N2Engine::Rigidbody made the members below name a
    // different type depending on what a file had included first, and made 'Rigidbody' ambiguous
    class Rigidbody;
    class ICollider;


    /**
     * Single contact point in a collision
     */
    struct ContactPoint
    {
        Math::Vector3 point;     // World-space contact position
        Math::Vector3 normal;    // Surface normal at contact (points from other to this)
        float separation;        // Penetration depth (negative = overlap)
        float normalImpulse;     // Impulse magnitude along normal
        float tangentImpulse[2]; // Friction impulses (tangent directions)

        ContactPoint()
            : point(Math::Vector3::Zero), normal(Math::Vector3::Zero), separation(0.0f), normalImpulse(0.0f), tangentImpulse{0.0f, 0.0f}
        {
        }
    };

    /**
     * Collision data for OnCollisionEnter/Stay/Exit
     * Contains references to the colliding objects
     */
    struct Collision
    {
        // Direct references to the colliding objects
        GameObject *gameObject = nullptr;      // The GameObject receiving this callback
        Rigidbody *rigidbody = nullptr;        // The Rigidbody receiving this callback
        GameObject *otherGameObject = nullptr; // The other GameObject in the collision
        Rigidbody *otherRigidbody = nullptr;   // The other Rigidbody (nullptr if static)
        ICollider *collider = nullptr;         // This object's collider that made first contact
        ICollider *otherCollider = nullptr;    // The other object's collider in that contact

        // Collision details
        std::vector<ContactPoint> contacts; // All contact points (can be multiple)
        Math::Vector3 relativeVelocity;     // Relative velocity at impact
        Math::Vector3 impulse;              // Total impulse applied this frame

        Collision()
            : relativeVelocity(Math::Vector3::Zero), impulse(Math::Vector3::Zero)
        {
        }

        // Helper to get the first contact point (most common use case)
        [[nodiscard]] const ContactPoint *GetContact() const
        {
            return contacts.empty() ? nullptr : &contacts[0];
        }

        // Get average contact point
        [[nodiscard]] Math::Vector3 GetAverageContactPoint() const
        {
            if (contacts.empty())
                return Math::Vector3::Zero;

            Math::Vector3 avg = Math::Vector3::Zero;
            for (const auto &contact : contacts)
            {
                avg = avg + contact.point;
            }
            return avg / static_cast<float>(contacts.size());
        }
    };

    /**
     * Trigger overlap data for OnTriggerEnter/Stay/Exit
     * Simpler than Collision - just object references
     */
    struct Trigger
    {
        // Direct references to the objects
        GameObject *gameObject = nullptr;      // The GameObject receiving this callback
        Rigidbody *rigidbody = nullptr;        // The Rigidbody receiving this callback
        GameObject *otherGameObject = nullptr; // The other GameObject that entered/exited
        Rigidbody *otherRigidbody = nullptr;   // The other Rigidbody (nullptr if static)
        ICollider *collider = nullptr;         // This object's collider involved (first one touching)
        ICollider *otherCollider = nullptr;    // The other object's collider involved

        Trigger() = default;
    };

}