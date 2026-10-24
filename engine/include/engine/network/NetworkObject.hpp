#pragma once

#include <string>

#include "engine/Component.hpp"
#include "engine/network/NetworkId.hpp"
#include "engine/network/InterestManagement.hpp"

namespace N2Engine::Network
{
    class NetworkWorld;

    /**
     * Marks a GameObject as part of the replicated world (netcode stage B1). A scene-placed object with a NetworkObject
     * is matched between the server and every client by its GameObject UUID (both load the same scene file), and an
     * object the server spawns from a registered prefab travels as (prefab index, net id, owner). The component itself
     * saves nothing beyond the base keys: the id, owner and prefab are the session's, set by a NetworkWorld, never
     * stored in a scene or a prefab.
     *
     * At most one per object. A prefab that is spawned over the network must have exactly one, on its root.
     *
     * Before a NetworkWorld has taken the object (scene matching or a spawn) it has no id, no owner and replicates
     * nothing. Field replication is not part of this stage: the values of the object's components are not sent, only
     * its existence (spawn and despawn).
     */
    class NetworkObject final : public Component
    {
    public:
        explicit NetworkObject(GameObject &gameObject);
        ~NetworkObject() override;

        [[nodiscard]] std::string GetTypeName() const override;

        /// Tells the world the object is gone (the server replicates that as a despawn)
        void OnDestroy() override;

        /// 0 (invalid) until a NetworkWorld has registered the object
        [[nodiscard]] NetworkId GetNetworkId() const noexcept { return _id; }
        [[nodiscard]] bool HasNetworkId() const noexcept { return _id.IsValid(); }
        /// The client that owns the object; kServerClientId (0) for an object the server owns, which is every scene
        /// object. Fixed at spawn: nothing changes an owner in this stage, and a client id is never reused, so an object
        /// whose owner left keeps naming a client that no longer exists (what to do with it is the game's choice).
        [[nodiscard]] ClientId GetOwner() const noexcept { return _owner; }
        /// True for an object a world spawned from a prefab, false for one matched from the scene
        [[nodiscard]] bool IsSpawned() const noexcept { return _spawned; }
        /// The prefab a spawned object was made from; empty for a scene object
        [[nodiscard]] const std::string &GetPrefabName() const noexcept { return _prefabName; }
        /// True while a world has the object registered
        [[nodiscard]] bool IsRegistered() const noexcept { return _world != nullptr; }

        static constexpr bool IsSingleton = true;

        bool SetInterestPolicy(InterestPolicy policy);
        [[nodiscard]] const InterestPolicy &GetInterestPolicy() const { return _interest; }

    private:
        friend class NetworkWorld;

        /// Leaves the world it is registered with, telling it the object is gone. Idempotent.
        void Leave();

        NetworkWorld *_world = nullptr;
        NetworkId _id;
        ClientId _owner = kServerClientId;
        bool _spawned = false;
        std::string _prefabName;
        InterestPolicy _interest;
    };
}
