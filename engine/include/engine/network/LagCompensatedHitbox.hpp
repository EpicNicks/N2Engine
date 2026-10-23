#pragma once
#include "engine/network/LagCompensation.hpp"
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::Network
{
    // Opt-in server hitbox, separate from physical collision geometry. One per NetworkObject.
    class LagCompensatedHitbox final : public SerializableComponent
    {
      public:
        explicit LagCompensatedHitbox(GameObject &object);
        std::string GetTypeName() const override
        {
            return "LagCompensatedHitbox";
        }
        std::optional<HistoricalHitbox> Snapshot(NetworkId id) const;
        static constexpr bool IsSingleton = true;

      private:
        std::string _shape = "sphere";
        Math::Vector3 _centre;
        Math::Vector3 _endpoint{0, 1, 0};
        Math::Vector3 _halfExtents{0.5f, 0.5f, 0.5f};
        float _radius = 0.5f;
    };
} // namespace N2Engine::Network
