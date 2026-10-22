#include "engine/network/NetworkObject.hpp"

#include "engine/network/NetworkWorld.hpp"

namespace N2Engine::Network
{
    NetworkObject::NetworkObject(GameObject &gameObject) : Component(gameObject) {}

    NetworkObject::~NetworkObject()
    {
        Leave();
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
