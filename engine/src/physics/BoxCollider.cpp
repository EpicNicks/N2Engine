#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ReferenceResolver.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Physics
{
    REGISTER_COMPONENT(BoxCollider)

    BoxCollider::BoxCollider(GameObject& gameObject)
        : ICollider(gameObject)
    {
        RegisterMember(NAMEOF(_halfExtents), _halfExtents);
    }

    std::string BoxCollider::GetTypeName() const
    {
        return NAMEOF(BoxCollider);
    }

    void BoxCollider::SetSize(const Math::Vector3& size)
    {
        SetHalfExtents(size * 0.5f);
    }

    Math::Vector3 BoxCollider::GetSize() const
    {
        return _halfExtents * 2.0f;
    }

    void BoxCollider::SetHalfExtents(const Math::Vector3& halfExtents)
    {
        if (_halfExtents == halfExtents)
            return;
            
        _halfExtents = halfExtents;
        UpdateShapeGeometry();
    }

    Math::Vector3 BoxCollider::GetHalfExtents() const
    {
        return _halfExtents;
    }

    void BoxCollider::AttachShape(IPhysicsBackend* backend)
    {
        if (!backend || !GetHandle().IsValid())
            return;

        backend->AddBoxCollider(
            GetHandle(),
            _halfExtents,
            GetOffset(),
            GetMaterial()
        );
    }

    void BoxCollider::UpdateShapeGeometry()
    {
        if (!GetHandle().IsValid())
            return;

        auto* backend = Application::GetInstance().Get3DPhysicsBackend();
        if (!backend)
            return;

        backend->UpdateBoxCollider(
            GetHandle(),
            this,
            _halfExtents,
            GetOffset(),
            GetMaterial()
        );
    }
}