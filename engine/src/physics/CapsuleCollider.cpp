#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

namespace N2Engine::Physics
{
    CapsuleCollider::CapsuleCollider(GameObject& gameObject)
        : ICollider(gameObject)
    {
    }

    std::string CapsuleCollider::GetTypeName() const
    {
        return NAMEOF(CapsuleCollider);
    }

    nlohmann::json CapsuleCollider::Serialize() const
    {
        nlohmann::json j = ICollider::Serialize();
        j["radius"] = _radius;
        j["height"] = _height;
        return j;
    }

    void CapsuleCollider::Deserialize(const nlohmann::json& j, ReferenceResolver* resolver)
    {
        ICollider::Deserialize(j, resolver);
        
        if (j.contains("radius"))
        {
            _radius = j["radius"];
        }
        
        if (j.contains("height"))
        {
            _height = j["height"];
        }
    }

    void CapsuleCollider::SetRadius(float radius)
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

    void CapsuleCollider::SetHeight(float height)
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

    REGISTER_COMPONENT(CapsuleCollider)
}