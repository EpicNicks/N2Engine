#include "engine/network/LagCompensatedHitbox.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/NetworkTransform.hpp"
#include <algorithm>
#include <cmath>
namespace N2Engine::Network
{
    LagCompensatedHitbox::LagCompensatedHitbox(GameObject &object) : SerializableComponent(object)
    {
        RegisterMember("shape", _shape).Tooltip("sphere, capsule or box; historical query geometry only");
        RegisterMember("centre", _centre);
        RegisterMember("endpoint", _endpoint).Tooltip("Capsule second end-sphere centre in local space");
        RegisterMember("halfExtents", _halfExtents);
        RegisterMember("radius", _radius).Range(0.0001, 1e6);
    }
    std::optional<HistoricalHitbox> LagCompensatedHitbox::Snapshot(NetworkId id) const
    {
        const auto &go = GetGameObject();
        const auto *p = go.GetPositionable();
        if (!id.IsValid() || !IsActive() || !p)
            return {};
        HistoricalHitbox box;
        box.object = id;
        if (_shape == "sphere")
            box.shape = HistoricalHitbox::Shape::Sphere;
        else if (_shape == "capsule")
            box.shape = HistoricalHitbox::Shape::Capsule;
        else if (_shape == "box")
            box.shape = HistoricalHitbox::Shape::Box;
        else
            return {};
        const auto scale = p->GetScale();
        const Math::Vector3 absScale{std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)};
        auto scaled = [&](const Math::Vector3 &v) {
            return Math::Vector3{v.x * scale.x, v.y * scale.y, v.z * scale.z};
        };
        box.rotation = p->GetRotation();
        box.centre = p->GetPosition() + box.rotation * scaled(_centre);
        box.endpoint = p->GetPosition() + box.rotation * scaled(_endpoint);
        box.radius = _radius * std::max({absScale.x, absScale.y, absScale.z});
        box.halfExtents =
            Math::Vector3{_halfExtents.x * absScale.x, _halfExtents.y * absScale.y, _halfExtents.z * absScale.z};
        box.layers = 1u << static_cast<unsigned>(go.GetLayer());
        if (const auto *transform = go.GetComponent<NetworkTransform>())
            box.teleportSerial = transform->GetTeleportSerial();
        if (!LagCompensation::IsValid(box))
            return {};
        return box;
    }
} // namespace N2Engine::Network
