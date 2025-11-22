#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

namespace N2Engine::Physics
{
    SphereCollider::SphereCollider(GameObject& gameObject)
        : ICollider(gameObject)
    {
    }

    std::string SphereCollider::GetTypeName() const
    {
        return NAMEOF(SphereCollider);
    }

    nlohmann::json SphereCollider::Serialize() const
    {
        nlohmann::json j = ICollider::Serialize();
        j["radius"] = _radius;
        return j;
    }

    void SphereCollider::Deserialize(const nlohmann::json& j, ReferenceResolver* resolver)
    {
        ICollider::Deserialize(j, resolver);
        if (j.contains("radius"))
        {
            _radius = j["radius"];
        }
    }

    void SphereCollider::SetRadius(float radius)
    {
        if (_radius == radius)
            return;
            
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

    REGISTER_COMPONENT(SphereCollider)
}