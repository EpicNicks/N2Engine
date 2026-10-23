#include "engine/network/NetworkObject.hpp"

#include "engine/network/NetworkWorld.hpp"
#include <cmath>

namespace N2Engine::Network
{
    NetworkObject::NetworkObject(GameObject &gameObject) : Component(gameObject) {}

    NetworkObject::~NetworkObject()
    {
        Leave();
    }

    bool NetworkObject::SetInterestPolicy(InterestPolicy policy)
    {
        if (!std::isfinite(policy.radius) || policy.radius < 0 || policy.radius > 1e6f ||
            !std::isfinite(policy.priority) || policy.priority < 0.001f || policy.priority > 1000 ||
            (policy.relevance != Relevance::Always && policy.relevance != Relevance::OwnerOnly &&
             policy.relevance != Relevance::Radius)) return false;
        _interest = policy;
        return true;
    }

    std::string NetworkObject::GetTypeName() const
    {
        return "NetworkObject";
    }

    void NetworkObject::OnDestroy()
    {
        Leave();
    }

    void NetworkObject::Leave()
    {
        if (_world == nullptr)
        {
            return;
        }
        NetworkWorld *const world = _world;
        _world = nullptr;
        world->ObjectGone(*this);
    }
}
