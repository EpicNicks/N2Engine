#pragma once

// One 2D physics world, owned by its Scene and created by the first 2D component that attaches, so a scene without any has no
// world and no cost. It is abstract so that the only code naming Box2D is Physics2DWorld.cpp: with N2ENGINE_USE_BOX2D=OFF,
// Create returns null, the components cope with that, and no Box2D code is in the binary (docs/physics2d.html#stripping).
// Main thread only.

#include <cstdint>
#include <memory>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics2d/Physics2DTypes.hpp"

namespace N2Engine::Physics2D
{
    using Physics::PhysicsBodyHandle;
    using Physics::QueryTriggers;

    struct BodyDesc2D
    {
        BodyType2D type = BodyType2D::Static;
        GameObject *owner = nullptr;
        Rigidbody2D *rigidbody = nullptr; // null for a collider's own static body
        Math::Vector2 linearVelocity;
        float angularVelocity = 0.0f;
        float linearDamping = 0.0f;
        float angularDamping = 0.0f;
        float gravityScale = 1.0f;
        bool fixedRotation = false;
        bool bullet = false;
        bool enabled = true;
    };

    struct ShapeDesc2D
    {
        enum class Kind : std::uint8_t
        {
            Box,
            Circle,
            Polygon
        };

        Kind kind = Kind::Box;
        Math::Vector2 offset;
        Math::Vector2 halfExtents{0.5f, 0.5f}; // Box
        float radius = 0.5f;                   // Circle
        std::vector<Math::Vector2> vertices;   // Polygon
        bool isTrigger = false;
        Physics::PhysicsMaterial material = Physics::PhysicsMaterial::Default();
        float density = 1.0f;
        float surfaceSpeed = 0.0f;
        int layer = 0;
    };

    class Physics2DWorld
    {
    public:
        /// A new world, or null in a build without Box2D (which warns once)
        [[nodiscard]] static std::shared_ptr<Physics2DWorld> Create();
        /// Re-reads Layers' collision matrix into every shape of every live world. Layers calls it when the matrix changes.
        static void RefreshAllCollisionMatrices();
        /// The gravity new worlds start with, and every live world's from now on
        static void SetGravity(const Math::Vector2 &gravity);
        [[nodiscard]] static Math::Vector2 GetGravity();

        virtual ~Physics2DWorld() = default;

        // ===== The fixed step, in Application::PhysicsUpdate's order =====
        /// Applies transforms the engine changed since the last step
        virtual void ApplyPendingChanges() = 0;
        /// Simulates, writes moved bodies back to their transforms, then delivers events. No engine or game code runs while
        /// Box2D is stepping; events are copied before the first handler runs.
        virtual void Step(float deltaTime) = 0;

        // ===== Bodies =====
        virtual PhysicsBodyHandle CreateBody(const BodyDesc2D &desc) = 0;
        /// Pairs through the body end with an Exit in the next dispatch, on both sides
        virtual void DestroyBody(PhysicsBodyHandle body) = 0;
        virtual void SetBodyEnabled(PhysicsBodyHandle body, bool enabled) = 0;
        virtual void MarkTransformDirty(PhysicsBodyHandle body) = 0;
        [[nodiscard]] virtual bool IsSyncingTransforms() const = 0;

        virtual void SetBodyType(PhysicsBodyHandle body, BodyType2D type) = 0;
        virtual void SetLinearVelocity(PhysicsBodyHandle body, const Math::Vector2 &velocity) = 0;
        [[nodiscard]] virtual Math::Vector2 GetLinearVelocity(PhysicsBodyHandle body) const = 0;
        virtual void SetAngularVelocity(PhysicsBodyHandle body, float radiansPerSecond) = 0;
        [[nodiscard]] virtual float GetAngularVelocity(PhysicsBodyHandle body) const = 0;
        virtual void SetDamping(PhysicsBodyHandle body, float linear, float angular) = 0;
        virtual void SetGravityScale(PhysicsBodyHandle body, float scale) = 0;
        virtual void SetFixedRotation(PhysicsBodyHandle body, bool fixed) = 0;
        virtual void SetBullet(PhysicsBodyHandle body, bool bullet) = 0;
        virtual void AddForce(PhysicsBodyHandle body, const Math::Vector2 &force) = 0;
        virtual void AddImpulse(PhysicsBodyHandle body, const Math::Vector2 &impulse) = 0;
        virtual void AddTorque(PhysicsBodyHandle body, float torque) = 0;
        virtual void AddAngularImpulse(PhysicsBodyHandle body, float impulse) = 0;
        /// autoMass: the mass comes from the solid shapes' density. Otherwise `mass` is authoritative and the inertia is rescaled.
        virtual void SetMass(PhysicsBodyHandle body, bool autoMass, float mass) = 0;
        [[nodiscard]] virtual float GetMass(PhysicsBodyHandle body) const = 0;

        // ===== Shapes, one per collider =====
        /// Adds or updates the collider's shape; false, with a warning, for geometry Box2D cannot use. Changing the trigger flag
        /// re-creates the shape (Box2D cannot change it), so its pairs end with an Exit and start again if still overlapping.
        virtual bool SetCollider(PhysicsBodyHandle body, Collider2D *collider, const ShapeDesc2D &desc) = 0;
        virtual void RemoveCollider(Collider2D *collider) = 0;

        // ===== Queries, following Physics::QueryRules =====
        virtual bool Raycast(const Math::Vector2 &origin, const Math::Vector2 &direction, RaycastHit2D &hit, float maxDistance,
                             std::uint32_t layerMask, QueryTriggers triggers) = 0;
        virtual int RaycastAll(const Math::Vector2 &origin, const Math::Vector2 &direction, std::vector<RaycastHit2D> &hits,
                               float maxDistance, std::uint32_t layerMask, QueryTriggers triggers) = 0;
        virtual bool CircleCast(const Math::Vector2 &origin, float radius, const Math::Vector2 &direction, RaycastHit2D &hit,
                                float maxDistance, std::uint32_t layerMask, QueryTriggers triggers) = 0;
        virtual bool BoxCast(const Math::Vector2 &origin, const Math::Vector2 &halfExtents, float angleRadians,
                             const Math::Vector2 &direction, RaycastHit2D &hit, float maxDistance, std::uint32_t layerMask,
                             QueryTriggers triggers) = 0;
        virtual int OverlapCircle(const Math::Vector2 &center, float radius, std::vector<OverlapHit2D> &hits,
                                  std::uint32_t layerMask, QueryTriggers triggers) = 0;
        virtual int OverlapBox(const Math::Vector2 &center, const Math::Vector2 &halfExtents, float angleRadians,
                               std::vector<OverlapHit2D> &hits, std::uint32_t layerMask, QueryTriggers triggers) = 0;

        /// Hash of the whole simulation, for tests and later for desync detection
        [[nodiscard]] virtual std::uint64_t HashState() const = 0;
    };
}
