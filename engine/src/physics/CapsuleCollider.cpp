#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

namespace N2Engine::Physics
{
    REGISTER_COMPONENT(CapsuleCollider)

    CapsuleCollider::CapsuleCollider(GameObject& gameObject)
        : ICollider(gameObject)
    {
        RegisterMember(NAMEOF(_radius), _radius);
        RegisterMember(NAMEOF(_height), _height);
    }

    std::string CapsuleCollider::GetTypeName() const
    {
        return NAMEOF(CapsuleCollider);
    }

    void CapsuleCollider::SetRadius(const float radius)
    {
        if (_radius == radius)
            return;
            
        _radius = radius;
        UpdateShapeGeometry();
    }

    float CapsuleCollider::GetRadius() const
    {
        return _radius;
    }

    void CapsuleCollider::SetHeight(const float height)
    {
        if (_height == height)
            return;
            
        _height = height;
        UpdateShapeGeometry();
    }

    float CapsuleCollider::GetHeight() const
    {
        return _height;
    }

    void CapsuleCollider::AttachShape(IPhysicsBackend* backend)
    {
        if (!backend || !GetHandle().IsValid())
            return;

        backend->AddCapsuleCollider(
            GetHandle(),
            _radius,
            _height,
            GetOffset(),
            GetMaterial()
        );
    }

    void CapsuleCollider::UpdateShapeGeometry()
    {
        if (!GetHandle().IsValid())
            return;

        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        backend->UpdateCapsuleCollider(
            GetHandle(),
            this,
            _radius,
            _height,
            GetOffset(),
            GetMaterial()
        );
    }
}