#pragma once

#include <math/Vector3.hpp>
#include <nlohmann/json.hpp>

#include "engine/physics/ICollider.hpp"

namespace N2Engine
{
    class ReferenceResolver;
}

namespace N2Engine::Physics
{
    class IPhysicsBackend;

    class BoxCollider final : public ICollider
    {
    public:
        explicit BoxCollider(GameObject& gameObject);

        [[nodiscard]] std::string GetTypeName() const override;
        [[nodiscard]] nlohmann::json Serialize() const override;
        void Deserialize(const nlohmann::json& j, ReferenceResolver* resolver) override;

        void SetSize(const Math::Vector3& size);
        [[nodiscard]] Math::Vector3 GetSize() const;

        void SetHalfExtents(const Math::Vector3& halfExtents);
        [[nodiscard]] Math::Vector3 GetHalfExtents() const;

    protected:
        void AttachShape(IPhysicsBackend* backend) override;
        void UpdateShapeGeometry() override;

    private:
        Math::Vector3 _halfExtents{0.5f, 0.5f, 0.5f};
    };
}