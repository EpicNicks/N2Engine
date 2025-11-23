#pragma once

#include <nlohmann/json.hpp>
#include "engine/physics/ICollider.hpp"

namespace N2Engine
{
    class ReferenceResolver;
}

namespace N2Engine::Physics
{
    class IPhysicsBackend;

    class SphereCollider final : public ICollider
    {
    public:
        explicit SphereCollider(GameObject& gameObject);

        [[nodiscard]] std::string GetTypeName() const override;

        void SetRadius(float radius);
        [[nodiscard]] float GetRadius() const;

    protected:
        void AttachShape(IPhysicsBackend* backend) override;
        void UpdateShapeGeometry() override;

    private:
        float _radius = 0.5f;
    };
}