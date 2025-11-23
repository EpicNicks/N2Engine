#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

namespace N2Engine::Physics
{
    REGISTER_COMPONENT(SphereCollider)

    SphereCollider::SphereCollider(GameObject& gameObject)
        : ICollider(gameObject)
    {
        RegisterMember(NAMEOF(_radius), _radius);
    }

    std::string SphereCollider::GetTypeName() const
    {
        return NAMEOF(SphereCollider);
    }

    void SphereCollider::SetRadius(const float radius)
    {
        if (_radius == radius)
        {
            return;
        }
            
        _radius = radius;
        UpdateShapeGeometry();
    }

    float SphereCollider::GetRadius() const
    {
        return _radius;
    }

    void SphereCollider::AttachShape(IPhysicsBackend* backend)
    {
        if (!backend || !GetHandle().IsValid())
            return;

        backend->AddSphereCollider(
            GetHandle(),
            _radius,
            GetOffset(),
            GetMaterial()
        );
    }

    void SphereCollider::UpdateShapeGeometry()
    {
        if (!GetHandle().IsValid())
            return;

        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        backend->UpdateSphereCollider(
            GetHandle(),
            this,
            _radius,
            GetOffset(),
            GetMaterial()
        );
    }
}