#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "engine/network/NetworkId.hpp"
#include "engine/network/NetworkObject.hpp"

namespace N2Engine
{
    class Scene;
}

namespace N2Engine::Network
{
    /**
     * The registry of one scene's NetworkObjects: net id to object, the matching of scene-placed objects, and the
     * instantiating and destroying of spawned ones. It is the part of the network layer that touches the scene; the
     * server (NetworkServer) and the client (NetworkClient) each drive one and add the messages. One scene at a time:
     * it is bound to the scene it was given, and when that scene is freed (a scene change) it is lost (HasScene is
     * false) and nothing it holds may be used. Single-threaded, like the scene.
     *
     * Scene matching (BindScene): every GameObject of the scene (active or not, attached or not) that has a
     * NetworkObject is a scene object. They are sorted by the bytes of their GameObject UUID and given ids 1, 2, 3 ...
     * in that order, which both sides compute from the same scene file, so nothing about them travels on the wire (the
     * fingerprint check proves the files are the same). A UUID used twice, or more than kMaxNetworkObjects objects, fails
     * the bind and leaves the world empty.
     *
     * The world never frees a NetworkObject it does not own: they belong to their GameObjects. An object leaves the
     * world when its GameObject is destroyed (its destroyed handler is told), when the world destroys it (Despawn, no
     * handler call) or when the world is reset or destroyed (no call).
     */
    class NetworkWorld
    {
    public:
        /// Called when an object registered here is destroyed by anyone but the world's own Despawn: its id, and whether
        /// it was a spawned object (false: a scene object). It runs inside the component's OnDestroy or destructor, so it
        /// must only note the fact down.
        using DestroyedHandler = std::function<void(NetworkId id, bool wasSpawned)>;

        NetworkWorld() = default;
        ~NetworkWorld();
        NetworkWorld(const NetworkWorld &) = delete;
        NetworkWorld &operator=(const NetworkWorld &) = delete;

        /// Matches the scene's NetworkObjects (see the class comment), replacing whatever the world held. Returns the
        /// number of scene objects, or what is wrong with the scene.
        [[nodiscard]] std::expected<std::uint32_t, std::string> BindScene(Scene &scene);

        /// Forgets the scene and every object (they keep existing in their scene, as plain objects with no id)
        void Reset();

        /// True from a successful BindScene until the scene is freed
        [[nodiscard]] bool HasScene() const noexcept;
        /// The bound scene, or nullptr when there is none or it has been freed
        [[nodiscard]] Scene *GetScene() const noexcept;
        /// The number of scene objects the last bind matched; their ids are 1 to this
        [[nodiscard]] std::uint32_t SceneObjectCount() const noexcept { return _sceneObjectCount; }

        [[nodiscard]] NetworkObject *Find(NetworkId id) const;
        /// Objects registered now (scene and spawned)
        [[nodiscard]] std::size_t Count() const noexcept { return _objects.size(); }
        /// Net id to object, ascending by id
        [[nodiscard]] const std::map<std::uint32_t, NetworkObject *> &Objects() const noexcept { return _objects; }

        /// The next id never given out in this session: the counter only rises. An invalid id once the 32-bit counter
        /// is exhausted (the server then refuses to spawn). Used by the server; a client takes the ids it is told.
        [[nodiscard]] NetworkId AllocateId();

        /**
         * Makes an instance of the registered prefab `prefabName`, adds it to the bound scene as a root object and
         * registers its NetworkObject under `id` with `owner`. Fails (and leaves nothing behind) when there is no scene,
         * the id is invalid or in use, kMaxNetworkObjects objects are registered, the prefab can't be instantiated, or
         * the instance does not have exactly one NetworkObject, on its root.
         */
        [[nodiscard]] std::expected<NetworkObject *, std::string> SpawnInstance(NetworkId id, std::string_view prefabName,
                                                                                ClientId owner);

        struct DespawnResult
        {
            bool found = false;
            bool wasSpawned = false;
        };

        /// Unregisters the object and destroys its GameObject (the scene frees it at its next destroy pass). The
        /// destroyed handler is not called: the caller knows. Not found: nothing happens.
        DespawnResult Despawn(NetworkId id);

        /// Replaces the handler (an empty function removes it)
        void SetDestroyedHandler(DestroyedHandler handler) { _destroyedHandler = std::move(handler); }

    private:
        friend class NetworkObject;

        /// An object registered here is being destroyed or freed
        void ObjectGone(NetworkObject &object);

        Scene *_scene = nullptr;
        std::weak_ptr<const bool> _sceneLifetime;
        std::uint32_t _sceneObjectCount = 0;
        std::uint32_t _nextId = 1;
        std::map<std::uint32_t, NetworkObject *> _objects;
        DestroyedHandler _destroyedHandler;
    };
}
