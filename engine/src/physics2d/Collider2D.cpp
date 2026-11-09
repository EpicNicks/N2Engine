#include "engine/physics2d/Collider2D.hpp"

#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Physics2D
{
    Collider2D::Collider2D(GameObject &gameObject)
        : SerializableComponent(gameObject)
    {
        RegisterMember(NAMEOF(_isTrigger), _isTrigger);
        RegisterMember(NAMEOF(_material), _material);
        RegisterMember(NAMEOF(_offset), _offset);
        RegisterMember(NAMEOF(_density), _density);
        RegisterMember(NAMEOF(_surfaceSpeed), _surfaceSpeed);
    }

    void Collider2D::OnAttach()
    {
        // Re-attaching (a Rigidbody2D was added or removed, or the object changed scene): off the old body first
        if (const auto old = _world.lock(); old && _handle.IsValid())
        {
            old->RemoveCollider(this);
            if (_ownsBody)
            {
                old->DestroyBody(_handle);
            }
        }
        _handle = Physics::INVALID_PHYSICS_HANDLE;
        _ownsBody = false;
        _shapeAttached = false;
        _world.reset();

        Scene *scene = GetGameObject().GetScene();
        const std::shared_ptr<Physics2DWorld> world = scene ? scene->GetOrCreatePhysics2DWorld() : nullptr;
        if (!world)
        {
            return; // a build without Box2D: the component keeps its data
        }
        if (!GetGameObject().GetPositionable())
        {
            GetGameObject().CreatePositionable();
        }
        _world = world;

        const Rigidbody2D *rigidbody = GetGameObject().GetComponent<Rigidbody2D>();
        if (rigidbody && !rigidbody->IsDestroyed() && rigidbody->GetHandle().IsValid() && rigidbody->GetWorld() == world)
        {
            _handle = rigidbody->GetHandle();
        }
        else
        {
            BodyDesc2D desc;
            desc.type = BodyType2D::Static;
            desc.owner = &GetGameObject();
            desc.enabled = GetGameObject().IsActiveInHierarchy();
            _handle = world->CreateBody(desc);
            _ownsBody = true;
        }
        SetShapeAttached(IsActive());
    }

    void Collider2D::OnDestroy()
    {
        if (const auto world = _world.lock(); world && _handle.IsValid())
        {
            world->RemoveCollider(this);
            if (_ownsBody)
            {
                world->DestroyBody(_handle);
            }
        }
        _world.reset();
        _handle = Physics::INVALID_PHYSICS_HANDLE;
        _ownsBody = false;
        _shapeAttached = false;
    }

    void Collider2D::OnEnable()
    {
        SetShapeAttached(IsActive());
    }

    void Collider2D::OnDisable()
    {
        SetShapeAttached(false);
    }

    void Collider2D::OnActiveFlagChanged()
    {
        SetShapeAttached(IsActive());
    }

    ShapeDesc2D Collider2D::Describe() const
    {
        ShapeDesc2D desc;
        DescribeGeometry(desc);
        desc.offset = _offset;
        desc.isTrigger = _isTrigger;
        desc.material = _material;
        desc.density = _density;
        desc.surfaceSpeed = _surfaceSpeed;
        desc.layer = GetGameObject().GetLayer();
        return desc;
    }

    void Collider2D::SetShapeAttached(const bool attached)
    {
        const auto world = _world.lock();
        if (!world || !_handle.IsValid() || attached == _shapeAttached)
        {
            return;
        }
        // An own static body follows the object's active state too
        if (_ownsBody)
        {
            world->SetBodyEnabled(_handle, GetGameObject().IsActiveInHierarchy());
        }
        if (attached)
        {
            world->SetCollider(_handle, this, Describe());
        }
        else
        {
            world->RemoveCollider(this);
        }
        _shapeAttached = attached;
    }

    void Collider2D::UpdateShape()
    {
        if (const auto world = _world.lock(); world && _handle.IsValid() && _shapeAttached)
        {
            world->SetCollider(_handle, this, Describe());
        }
    }

    void Collider2D::SetIsTrigger(const bool isTrigger)
    {
        _isTrigger = isTrigger;
        UpdateShape();
    }

    void Collider2D::SetMaterial(const Physics::PhysicsMaterial &material)
    {
        _material = material;
        UpdateShape();
    }

    void Collider2D::SetOffset(const Math::Vector2 &offset)
    {
        _offset = offset;
        UpdateShape();
    }

    void Collider2D::SetDensity(const float density)
    {
        _density = density;
        UpdateShape();
    }

    void Collider2D::SetSurfaceSpeed(const float speed)
    {
        _surfaceSpeed = speed;
        UpdateShape();
    }

    void Collider2D::OnTransformChanged() const
    {
        if (!_ownsBody)
        {
            return;
        }
        if (const auto world = _world.lock(); world && !world->IsSyncingTransforms(_handle))
        {
            world->MarkTransformDirty(_handle);
        }
    }

    void Collider2D::OnLayerChanged()
    {
        UpdateShape();
    }

    // ===== Box =====
    BoxCollider2D::BoxCollider2D(GameObject &gameObject)
        : Collider2D(gameObject)
    {
        RegisterMember(NAMEOF(_halfExtents), _halfExtents);
    }

    std::string BoxCollider2D::GetTypeName() const
    {
        return NAMEOF(BoxCollider2D);
    }

    void BoxCollider2D::SetSize(const Math::Vector2 &size)
    {
        SetHalfExtents(Math::Vector2(size.x * 0.5f, size.y * 0.5f));
    }

    void BoxCollider2D::SetHalfExtents(const Math::Vector2 &halfExtents)
    {
        _halfExtents = halfExtents;
        UpdateShape();
    }

    void BoxCollider2D::DescribeGeometry(ShapeDesc2D &desc) const
    {
        desc.kind = ShapeDesc2D::Kind::Box;
        desc.halfExtents = _halfExtents;
    }

    // ===== Circle =====
    CircleCollider2D::CircleCollider2D(GameObject &gameObject)
        : Collider2D(gameObject)
    {
        RegisterMember(NAMEOF(_radius), _radius);
    }

    std::string CircleCollider2D::GetTypeName() const
    {
        return NAMEOF(CircleCollider2D);
    }

    void CircleCollider2D::SetRadius(const float radius)
    {
        _radius = radius;
        UpdateShape();
    }

    void CircleCollider2D::DescribeGeometry(ShapeDesc2D &desc) const
    {
        desc.kind = ShapeDesc2D::Kind::Circle;
        desc.radius = _radius;
    }

    // ===== Polygon =====
    PolygonCollider2D::PolygonCollider2D(GameObject &gameObject)
        : Collider2D(gameObject)
    {
        RegisterMember(NAMEOF(_vertices), _vertices);
    }

    std::string PolygonCollider2D::GetTypeName() const
    {
        return NAMEOF(PolygonCollider2D);
    }

    void PolygonCollider2D::SetVertices(const std::vector<Math::Vector2> &vertices)
    {
        _vertices = vertices;
        UpdateShape();
    }

    void PolygonCollider2D::DescribeGeometry(ShapeDesc2D &desc) const
    {
        desc.kind = ShapeDesc2D::Kind::Polygon;
        desc.vertices = _vertices;
    }
}
