#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/NetworkId.hpp"
#include "engine/network/NetworkReplication.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/PrefabTable.hpp"
#include "engine/network/n2net/Connection.hpp"
#include "engine/network/n2net/Transport.hpp"

namespace N2Engine::Network
{
    /// The connection id of every world Connection (ConnectionConfig::connectionId). It is the same constant on both
    /// ends: it only filters stray packets, and over the secure layer every packet the Connection sees is already
    /// authenticated.
    inline constexpr std::uint32_t kConnectionId = 0x4E32'4E42; // "N2NB"

    /// The Connection settings both ends of a world connection use over `transport`: the same on both sides where the
    /// n2net layer needs them to agree (the reliable window, the first unit id, the connection id), the largest packet
    /// the transport takes, and a reliable message size of kMaxMessageBytes, so a peer that sends a larger message
    /// fails its own connection.
    [[nodiscard]] N2Net::ConnectionConfig MakeWorldConnectionConfig(std::uint32_t protocolId,
                                                                    const N2Net::ITransport &transport);

    struct NetworkServerOptions
    {
        /// Identifies the game and its build in every packet (ConnectionConfig::protocolId); the secure layer under the
        /// transport has its own and should be given the same
        std::uint32_t protocolId = 0;
        /// The most clients at once (handshaking and joined); a connection beyond it is dropped at once
        std::uint32_t maxClients = 16;
        /// A client that has not sent its accept this long after it connected is dropped
        std::uint64_t joinTimeoutMicros = 10'000'000;
        /// More messages than this from one client in one second is a violation and the client is dropped. The world
        /// messages a client sends are an accept (or a reject), once: this is a ceiling, not a rate anyone needs.
        std::uint32_t maxClientMessagesPerSecond = 32;
        /// The scene's resource path (informational text of the fingerprint) and the hash of its parsed content
        /// (HashSceneJson)
        std::string scenePath;
        std::uint64_t sceneHash = 0;
        /// The prefabs a spawn may name, frozen for this session (PrefabTable::Capture)
        PrefabTable prefabs;
        /// How often the server sends the replicated fields (netcode stage B2), in times per second: the setting
        /// network.authoritative.snapshotRate in a game. 0 turns state replication off; at most 240.
        std::uint32_t stateSendRateHz = 30;
        /// More state acknowledgements than this from one client in one second is a violation and the client is dropped.
        /// A client sends at most one per update it applied state in, so this is a ceiling, not a rate anyone needs.
        std::uint32_t maxClientAcksPerSecond = 240;
        /// The bounds of the state messages (see ServerReplicationOptions)
        ServerReplicationOptions replication;
    };

    /// Counters since the server was made. All are data: nothing here throws.
    struct NetworkServerStats
    {
        std::uint64_t clientsJoined = 0;
        /// Clients that sent a reject (they left because the fingerprint differed)
        std::uint64_t clientsRefused = 0;
        /// Clients the server dropped, for any of the reasons below
        std::uint64_t clientsDropped = 0;
        /// A message that does not decode (including a wrong wire version)
        std::uint64_t droppedMalformed = 0;
        /// A client sent a message only a server may send (Spawn, Despawn, the hello, WorldReady): clients never
        /// create or destroy anything
        std::uint64_t droppedUnauthorized = 0;
        /// A message that is legal but not now: an accept twice, a message on the unreliable channel
        std::uint64_t droppedProtocol = 0;
        std::uint64_t droppedRateLimited = 0;
        std::uint64_t droppedJoinTimeout = 0;
        /// The accept's fingerprint is not the server's
        std::uint64_t droppedBadFingerprint = 0;
        std::uint64_t droppedConnectionFailed = 0;
        /// A reliable send did not fit the client's queue
        std::uint64_t droppedSendFailed = 0;
        /// More clients than maxClients, or none could be given an id
        std::uint64_t droppedOverCapacity = 0;
        std::uint64_t spawnsSent = 0;
        std::uint64_t despawnsSent = 0;
    };

    /// What NetworkServer tells its observer about a client
    struct NetworkClientEvent
    {
        enum class Type : std::uint8_t
        {
            /// The client accepted the world and has been sent the join state
            Joined,
            /// The client is gone: it disconnected, or the server dropped it (`reason` says why)
            Left,
        };

        Type type = Type::Joined;
        ClientId client = kServerClientId;
        N2Net::PeerId peer = N2Net::kInvalidPeer;
        /// Left only: whether the client had joined before it left
        bool wasJoined = false;
        /// Left only: "disconnected" or what the server dropped it for
        std::string reason;
    };

    /**
     * The server side of the world protocol (netcode stage B1): it runs one n2net Connection per peer of a transport,
     * sends each client the fingerprint and, once the client accepted it, the join state (the scene objects destroyed
     * since the start, the live spawned objects), then replicates every spawn and despawn to the clients that joined.
     * Single-threaded; call Update every frame with a monotonic microsecond clock.
     *
     * Authority: only this object creates or destroys anything. A client can send nothing but an accept or a reject,
     * once each, on the reliable channel, and state acknowledgements on the unreliable one; a client that sends any other
     * message (a Spawn and a Despawn included), a malformed one, an unexpected channel, or more than
     * maxClientMessagesPerSecond (acknowledgements: maxClientAcksPerSecond), is dropped and nothing it sent has any
     * effect. Spawn and Despawn are methods of this class for the game's server code, not messages anyone can send to it.
     * The same holds for field values (netcode stage B2): the server reads the replicated fields of its own objects and
     * sends them (stateSendRateHz times a second, on the unreliable channel, as the difference to what each client
     * acknowledged: see ServerReplicator); nothing a client sends is ever read as a value, so a client cannot write the
     * replicated field of any object, its own included.
     *
     * The transport supplies authenticated peers (over an N2Server it is a SecureTransport: a peer exists once its
     * handshake succeeded). The server must not outlive the transport or the world, and the world must not be destroyed
     * while the server exists.
     */
    class NetworkServer
    {
    public:
        NetworkServer(N2Net::ITransport &transport, NetworkWorld &world, NetworkServerOptions options);
        ~NetworkServer();
        NetworkServer(const NetworkServer &) = delete;
        NetworkServer &operator=(const NetworkServer &) = delete;

        /// Called with every transport event before the server looks at it (a host that logs connections uses it).
        /// Runs inside Update's poll: it may not call Update, Poll or Disconnect.
        void SetTransportObserver(std::function<void(const N2Net::TransportEvent &)> observer)
        {
            _transportObserver = std::move(observer);
        }

        /// Called when a client joins or leaves. Runs inside Update, not inside the transport's poll; it may not call
        /// Update or DropPeer.
        void SetClientObserver(std::function<void(const NetworkClientEvent &)> observer)
        {
            _clientObserver = std::move(observer);
        }

        /// One step: polls the transport, handles what arrived, sends the despawns of objects the game destroyed, runs
        /// the timers (join timeout, failed connections), lets every connection send what is due, and disconnects the
        /// clients that were dropped. If the world's scene is gone, every client is dropped (a scene change is not
        /// replicated yet) and no new one is taken.
        void Update(std::uint64_t nowMicros);

        /// Spawns the registered prefab `prefabName` (it must be in the session's prefab table) for `owner` (the server,
        /// or a client that has joined): makes it, adds it to the scene and tells every client that has joined. Returns
        /// its net id.
        [[nodiscard]] std::expected<NetworkId, std::string> Spawn(std::string_view prefabName,
                                                                  ClientId owner = kServerClientId);

        /// Destroys the object and tells every client that has joined; false if there is no object with this id
        bool Despawn(NetworkId id);

        /// Drops the client on `peer` now, or forgets the peer if it is not a client: the host's way to refuse a peer it
        /// found wanting. Not for use inside the transport's poll (Update's observers).
        void DropPeer(N2Net::PeerId peer, std::string_view reason);

        /// Clients (handshaking and joined) and clients that joined
        [[nodiscard]] std::size_t ClientCount() const noexcept { return _clients.size(); }
        [[nodiscard]] std::size_t JoinedClientCount() const noexcept;
        [[nodiscard]] bool IsClientJoined(ClientId client) const;
        /// The ids of the clients that joined, ascending
        [[nodiscard]] std::vector<ClientId> JoinedClients() const;

        /// What the state replication has sent and what the clients acknowledged
        [[nodiscard]] const ServerReplicationStats &ReplicationStats() const noexcept { return _replicator.Stats(); }
        [[nodiscard]] const ServerReplicator &Replicator() const noexcept { return _replicator; }

        /// True once the world's scene went away; the server then takes no more clients
        [[nodiscard]] bool WorldLost() const noexcept { return _worldLost; }

        [[nodiscard]] const Fingerprint &GetFingerprint() const noexcept { return _fingerprint; }
        [[nodiscard]] const PrefabTable &Prefabs() const noexcept { return _options.prefabs; }
        [[nodiscard]] const NetworkServerStats &Stats() const noexcept { return _stats; }
        [[nodiscard]] NetworkWorld &World() noexcept { return _world; }

    private:
        enum class ClientState : std::uint8_t
        {
            /// Connected; the hello is sent; waiting for the accept
            Handshaking,
            /// Accepted: gets every spawn and despawn
            Joined,
        };

        struct Client
        {
            N2Net::PeerId peer = N2Net::kInvalidPeer;
            ClientId id = kServerClientId;
            ClientState state = ClientState::Handshaking;
            std::unique_ptr<N2Net::Connection> connection;
            std::uint64_t connectedAt = 0;
            std::uint64_t rateWindowStart = 0;
            std::uint32_t rateWindowCount = 0;
            std::uint64_t ackWindowStart = 0;
            std::uint32_t ackWindowCount = 0;
            bool dropping = false;
            std::string dropReason;
        };

        void HandleEvent(const N2Net::TransportEvent &event);
        void OnPeerConnected(N2Net::PeerId peer);
        void OnPeerDisconnected(N2Net::PeerId peer);
        void OnDatagram(N2Net::PeerId peer, std::span<const std::uint8_t> data);
        void OnMessage(Client &client, N2Net::MessageChannel channel, std::span<const std::uint8_t> message);
        void OnUnreliable(Client &client, std::span<const std::uint8_t> message);
        void OnAccept(Client &client, const Fingerprint &fingerprint);
        void SendJoinState(Client &client);
        bool Send(Client &client, const std::vector<std::uint8_t> &message);
        void Broadcast(const std::vector<std::uint8_t> &message);
        void FlushPendingDespawns();
        void SendState();
        void RunTimers();
        void SendPackets();
        void ApplyDrops();
        void MarkDrop(Client &client, std::string reason, std::uint64_t NetworkServerStats::*counter);
        bool AllowMessage(Client &client);
        bool AllowAck(Client &client);
        void NotifyLeft(const Client &client, std::string_view reason);

        N2Net::ITransport &_transport;
        NetworkWorld &_world;
        NetworkServerOptions _options;
        Fingerprint _fingerprint;
        NetworkServerStats _stats;
        ServerReplicator _replicator;
        std::uint64_t _nextStateAt = 0;
        std::uint64_t _now = 0;
        bool _worldLost = false;
        ClientId _nextClientId = 1;
        std::map<N2Net::PeerId, Client> _clients;
        /// Peers to disconnect once the poll has returned (a transport's Disconnect does nothing inside its Poll)
        std::vector<N2Net::PeerId> _refusedPeers;
        /// Ids of scene objects destroyed since the start, for a client that joins later
        std::set<std::uint32_t> _destroyedSceneIds;
        /// Objects the game destroyed that clients have not been told about yet
        std::vector<std::pair<std::uint32_t, bool>> _pendingDespawns;
        std::function<void(const N2Net::TransportEvent &)> _transportObserver;
        std::function<void(const NetworkClientEvent &)> _clientObserver;
    };
}
