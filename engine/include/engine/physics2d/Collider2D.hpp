#pragma once

#include <memory>
#include <vector>

#include <math/Vector2.hpp>

#include "engine/physics/PhysicsHandle.hpp"
#include "engine/physics/PhysicsMaterial.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Physics2D
{
    /// The base of the 2D colliders: one shape on the object's Rigidbody2D, or on a static body of its own when it has none.
    /// Sizes ignore the transform's scale, as the 3D colliders do. A disabled collider has no shape.
    class Collider2D : public SerializableComponent
    {
    public:
        explicit Collider2D(GameObject &gameObject);

        void OnAttach() override;
        void OnDestroy() override;
        void OnEnable() override;
        void OnDisable() override;

        /// A trigger reports overlaps and does not collide; it adds no mass. Toggling it ends and restarts its pairs.
        void SetIsTrigger(bool isTrigger);
        [[nodiscard]] bool IsTrigger() const { return _isTrigger; }

        /// dynamicFriction and restitution apply; Box2D has one friction coefficient, so staticFriction does not
        void SetMaterial(const Physics::PhysicsMaterial &material);
        [[nodiscard]] Physics::PhysicsMaterial GetMaterial() const { return _material; }

        void SetOffset(const Math::Vector2 &offset);
        [[nodiscard]] Math::Vector2 GetOffset() const { return _offset; }

        void SetDensity(float density);
        [[nodiscard]] float GetDensity() const { return _density; }

        /// Moves what touches this surface along it, for conveyors (units per second; docs/physics2d.html gives the direction)
        void SetSurfaceSpeed(float speed);
        [[nodiscard]] float GetSurfaceSpeed() const { return _surfaceSpeed; }

        void OnTransformChanged() const;
        /// GameObject::SetLayer calls it
        void OnLayerChanged();

        [[nodiscard]] Physics::PhysicsBodyHandle GetHandle() const { return _handle; }

    protected:
        void OnActiveFlagChanged() override;

        /// The subclass's geometry; the rest of the description is filled in here
        virtual void DescribeGeometry(ShapeDesc2D &desc) const = 0;
        /// Sends the current settings to the shape, when it has one
        void UpdateShape();

    private:
        void SetShapeAttached(bool attached);
        [[nodiscard]] ShapeDesc2D Describe() const;

        std::weak_ptr<Physics2DWorld> _world;
        Physics::PhysicsBodyHandle _handle;
        bool _ownsBody = false;
        bool _shapeAttached = false;

        bool _isTrigger = false;
        Physics::PhysicsMaterial _material = Physics::PhysicsMaterial::Default();
        Math::Vector2 _offset;
        float _density = 1.0f;
        float _surfaceSpeed = 0.0f;
    };

    class BoxCollider2D final : public Collider2D
    {
    public:
        explicit BoxCollider2D(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override;

        void SetSize(const Math::Vector2 &size);
        [[nodiscard]] Math::Vector2 GetSize() const { return Math::Vector2(_halfExtents.x * 2.0f, _halfExtents.y * 2.0f); }
        void SetHalfExtents(const Math::Vector2 &halfExtents);
        [[nodiscard]] Math::Vector2 GetHalfExtents() const { return _halfExtents; }

    protected:
        void DescribeGeometry(ShapeDesc2D &desc) const override;

    private:
        Math::Vector2 _halfExtents{0.5f, 0.5f};
    };

    class CircleCollider2D final : public Collider2D
    {
    public:
        explicit CircleCollider2D(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override;

        void SetRadius(float radius);
        [[nodiscard]] float GetRadius() const { return _radius; }

    protected:
        void DescribeGeometry(ShapeDesc2D &desc) const override;

    private:
        float _radius = 0.5f;
    };

    /// A convex polygon of 3 to 8 points. Points that are not convex are wrapped in their convex hull.
    class PolygonCollider2D final : public Collider2D
    {
    public:
        explicit PolygonCollider2D(GameObject &gameObject);
        [[nodiscard]] std::string GetTypeName() const override;

        void SetVertices(const std::vector<Math::Vector2> &vertices);
        [[nodiscard]] const std::vector<Math::Vector2> &GetVertices() const { return _vertices; }

    protected:
        void DescribeGeometry(ShapeDesc2D &desc) const override;

    private:
        // A unit square until set, so a new collider collides with something visible
        std::vector<Math::Vector2> _vertices{{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
    };
}
