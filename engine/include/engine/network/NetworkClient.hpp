#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkId.hpp"
#include "engine/network/NetworkProtocol.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/PrefabTable.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::Network
{
    enum class NetworkClientState : std::uint8_t
    {
        /// No server peer yet: the transport has not reported a link (over the secure layer: its handshake is running)
        Connecting,
        /// Linked; waiting for the server's hello
        AwaitingHello,
        /// The hello was accepted; the join state (the scene objects destroyed so far, the live spawned objects) is
        /// arriving
        Joining,
        /// The world matches the server's and follows its spawns and despawns
        Joined,
        /// The client refused to join (the fingerprint differs); see Error() and Refusal()
        Refused,
        /// The server closed the connection, or Leave was called
        Closed,
        /// A protocol violation by the server, or no hello in time; see Error()
        Failed,
    };

    [[nodiscard]] std::string_view ToString(NetworkClientState state) noexcept;

    struct NetworkClientOptions
    {
        /// The same protocol id the server's Connections use
        std::uint32_t protocolId = 0;
        /// This client's scene (an informational text) and the hash of its parsed content (HashSceneJson)
        std::string scenePath;
        std::uint64_t sceneHash = 0;
        /// The prefabs of this process, frozen (PrefabTable::Capture); the server's spawn names one by index
        PrefabTable prefabs;
        /// Linked, but no hello this long: the client fails
        std::uint64_t helloTimeoutMicros = 10'000'000;
        /// After refusing, the client keeps its connection up this long at most, to get its reject out (it leaves as soon
        /// as the reject was acknowledged)
        std::uint64_t refuseLingerMicros = 1'000'000;
    };

    struct NetworkClientStats
    {
        std::uint64_t spawnsApplied = 0;
        std::uint64_t despawnsApplied = 0;
        /// A despawn for an object this client does not have (it destroyed it itself, or it never had it)
        std::uint64_t despawnsUnknown = 0;
    };

    /// What NetworkClient tells its observer
    struct NetworkSessionEvent
    {
        enum class Type : std::uint8_t
        {
            /// The join state arrived and matched: the world is in step with the server
            Joined,
            Spawned,
            /// An object the server despawned was found and is being destroyed
            Despawned,
            /// The client refused to join; `text` is the error
            Refused,
            /// The connection ended without a violation
            Closed,
            Failed,
        };

        Type type = Type::Joined;
        NetworkId id;
        /// Spawned: the prefab's name
        std::string prefab;
        /// Spawned: the owner
        ClientId owner = kServerClientId;
        /// Refused, Closed and Failed: the error text
        std::string text;
    };

    /**
     * The client side of the world protocol (netcode stage B1): over one transport link to the server it runs an n2net
     * Connection, checks the server's fingerprint against its own and refuses to join, with an error that names the
     * difference, when they differ; otherwise it accepts, applies the join state and then every spawn and despawn the
     * server sends, into the world it was given (a NetworkWorld bound to the scene the client loaded). Headless: it
     * needs no window, renderer or engine tick, only a scene to put objects into.
     *
     * What it trusts: nothing it does not check. Every message is decoded with bounded lengths; a spawn must carry an id
     * above every scene id and above the last spawn's, name a prefab of the client's own table that has exactly one
     * NetworkObject on its root, and not exceed kMaxNetworkObjects; the join state must be as long as the server said.
     * Anything else fails the client (Failed, Error() says why) and it disconnects. A client never sends a spawn or a
     * despawn: it sends its accept or its reject, nothing else.
     *
     * Single-threaded; call Update every frame with a monotonic microsecond clock. It must not outlive the transport
     * or the world.
     */
    class NetworkClient
    {
    public:
        /// `world` must be bound to the client's scene already (NetworkWorld::BindScene): the fingerprint it sends and
        /// compares includes the number of scene objects
        NetworkClient(N2Net::ITransport &transport, NetworkWorld &world, NetworkClientOptions options);

        /// Called when something happens (see NetworkSessionEvent). Runs inside Update; it may not call Update.
        void SetEventObserver(std::function<void(const NetworkSessionEvent &)> observer)
        {
            _observer = std::move(observer);
        }

        /// One step: polls the transport, handles what arrived, checks the timers, lets the connection send what is due
        void Update(std::uint64_t nowMicros);

        /// Disconnects from the server (the next Update does it) and ends the session as Closed
        void Leave() noexcept { _leaveRequested = true; }

        [[nodiscard]] NetworkClientState State() const noexcept { return _state; }
        [[nodiscard]] bool IsJoined() const noexcept { return _state == NetworkClientState::Joined; }
        /// True once nothing more will happen: the session ended (Closed, Failed) or the refusal has been sent
        [[nodiscard]] bool IsFinished() const noexcept { return _finished; }
        /// What went wrong (Refused, Failed, Closed by the server); empty otherwise
        [[nodiscard]] const std::string &Error() const noexcept { return _error; }
        /// The reason of a refusal; None unless the state is Refused
        [[nodiscard]] JoinRefusal Refusal() const noexcept { return _refusal; }
        /// The id the server gave this client (0 until its hello)
        [[nodiscard]] ClientId LocalClientId() const noexcept { return _localClientId; }
        [[nodiscard]] const Fingerprint &LocalFingerprint() const noexcept { return _localFingerprint; }
        /// The server's, once its hello arrived
        [[nodiscard]] const std::optional<Fingerprint> &ServerFingerprint() const noexcept { return _serverFingerprint; }
        [[nodiscard]] const NetworkClientStats &Stats() const noexcept { return _stats; }
        [[nodiscard]] NetworkWorld &World() noexcept { return _world; }

    private:
        void HandleEvent(const N2Net::TransportEvent &event);
        void OnDatagram(std::span<const std::uint8_t> data);
        void OnMessage(N2Net::MessageChannel channel, std::span<const std::uint8_t> message);
        void OnHello(const ServerHelloMessage &hello);
        void OnSpawn(const SpawnMessage &spawn);
        void OnDespawn(const DespawnMessage &despawn);
        void OnWorldReady(const WorldReadyMessage &ready);
        void RefuseJoin(JoinRefusal reason, std::string text);
        void Fail(std::string text);
        void Notify(const NetworkSessionEvent &event);
        [[nodiscard]] bool Live() const noexcept;

        N2Net::ITransport &_transport;
        NetworkWorld &_world;
        NetworkClientOptions _options;
        Fingerprint _localFingerprint;
        std::optional<Fingerprint> _serverFingerprint;
        NetworkClientStats _stats;
        NetworkClientState _state = NetworkClientState::Connecting;
        std::string _error;
        JoinRefusal _refusal = JoinRefusal::None;
        ClientId _localClientId = kServerClientId;

        std::uint64_t _now = 0;
        N2Net::PeerId _serverPeer = N2Net::kInvalidPeer;
        std::unique_ptr<N2Net::Connection> _connection;
        std::uint64_t _helloWaitSince = 0;
        std::uint64_t _refusedAt = 0;
        std::uint32_t _lastSpawnId = 0;
        std::uint32_t _joinSpawns = 0;
        std::uint32_t _joinDespawns = 0;
        bool _leaveRequested = false;
        bool _disconnectPending = false;
        bool _finished = false;
        std::function<void(const NetworkSessionEvent &)> _observer;
    };
}
