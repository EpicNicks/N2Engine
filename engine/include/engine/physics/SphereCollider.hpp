#pragma once

#include "engine/physics/ICollider.hpp"

namespace N2Engine::Physics
{
    class SphereCollider : public ICollider
    {
    public:
        explicit SphereCollider(GameObject& gameObject)
            : ICollider(gameObject) {}

        void SetRadius(float radius)
        {
            if (_radius == radius) return;
            _radius = radius;
            UpdateShape();
        }

        [[nodiscard]] float GetRadius() const { return _radius; }

    protected:
        void AttachShape(IPhysicsBackend* backend) override
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

        void UpdateShape()
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

    private:
        float _radius = 0.5f;
    };
}