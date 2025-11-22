#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/Application.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

namespace N2Engine::Physics
{
    BoxCollider::BoxCollider(GameObject& gameObject)
        : ICollider(gameObject)
    {
    }

    std::string BoxCollider::GetTypeName() const
    {
        return NAMEOF(BoxCollider);
    }

    nlohmann::json BoxCollider::Serialize() const
    {
        nlohmann::json j = ICollider::Serialize();
        j["halfExtents"] = {_halfExtents.x, _halfExtents.y, _halfExtents.z};
        return j;
    }

    void BoxCollider::Deserialize(const nlohmann::json& j, ReferenceResolver* resolver)
    {
        ICollider::Deserialize(j, resolver);
        if (j.contains("halfExtents"))
        {
            const auto& he = j["halfExtents"];
            _halfExtents = Math::Vector3(he[0], he[1], he[2]);
        }
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

    REGISTER_COMPONENT(BoxCollider)
}