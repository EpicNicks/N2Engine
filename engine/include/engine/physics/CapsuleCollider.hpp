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

    class CapsuleCollider final : public ICollider
    {
    public:
        explicit CapsuleCollider(GameObject& gameObject);

        [[nodiscard]] std::string GetTypeName() const override;

        void SetRadius(float radius);
        [[nodiscard]] float GetRadius() const;

        void SetHeight(float height);
        [[nodiscard]] float GetHeight() const;

    protected:
        void AttachShape(IPhysicsBackend* backend) override;
        void UpdateShapeGeometry() override;

    private:
        float _radius = 0.5f;
        float _height = 2.0f;
    };
}