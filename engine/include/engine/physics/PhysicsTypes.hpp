#pragma once

#include <math/Vector3.hpp>
#include <vector>

namespace N2Engine
{
    class GameObject;
    class Rigidbody;
}

namespace N2Engine::Physics
{

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
            : point(Math::Vector3::Zero()), normal(Math::Vector3::Zero()), separation(0.0f), normalImpulse(0.0f), tangentImpulse{0.0f, 0.0f}
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

        // Collision details
        std::vector<ContactPoint> contacts; // All contact points (can be multiple)
        Math::Vector3 relativeVelocity;     // Relative velocity at impact
        Math::Vector3 impulse;              // Total impulse applied this frame

        Collision()
            : relativeVelocity(Math::Vector3::Zero()), impulse(Math::Vector3::Zero())
        {
        }

        // Helper to get the first contact point (most common use case)
        const ContactPoint *GetContact() const
        {
            return contacts.empty() ? nullptr : &contacts[0];
        }

        // Get average contact point
        Math::Vector3 GetAverageContactPoint() const
        {
            if (contacts.empty())
                return Math::Vector3::Zero();

            Math::Vector3 avg = Math::Vector3::Zero();
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

        Trigger() = default;
    };

}