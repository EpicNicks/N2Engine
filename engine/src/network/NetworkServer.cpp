#include "engine/network/NetworkServer.hpp"

#include <algorithm>
#include <format>
#include <optional>

#include "engine/Logger.hpp"
#include "engine/network/NetworkProtocol.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr std::uint64_t kMicrosPerSecond = 1'000'000;
        constexpr std::uint32_t kHardMaxClients = 1024;

        const char *FailureName(const N2Net::FailureReason reason)
        {
            switch (reason)
            {
            case N2Net::FailureReason::None:
                return "none";
            case N2Net::FailureReason::Timeout:
                return "timed out";
            case N2Net::FailureReason::ReassemblyStall:
                return "its reliable data stalled";
            case N2Net::FailureReason::MessageTooLarge:
                return "it sent a message that is too large";
            case N2Net::FailureReason::ProtocolViolation:
                return "protocol violation";
            }
            return "unknown";
        }
    }

    N2Net::ConnectionConfig MakeWorldConnectionConfig(const std::uint32_t protocolId, const N2Net::ITransport &transport)
    {
        N2Net::ConnectionConfig config;
        config.protocolId = protocolId;
        config.connectionId = kConnectionId;
        config.reliableStream = transport.IsReliableStream();
        config.maxPacketSize = static_cast<std::uint32_t>(std::min(transport.MaxPacketSize(), N2Net::kMaxPacketSize));
        config.maxMessageSize = static_cast<std::uint32_t>(kMaxMessageBytes);
        return config;
    }

    NetworkServer::NetworkServer(N2Net::ITransport &transport, NetworkWorld &world, NetworkServerOptions options)
        : _transport(transport), _world(world), _options(std::move(options))
    {
        _options.maxClients = std::clamp<std::uint32_t>(_options.maxClients, 1u, kHardMaxClients);
        _fingerprint = MakeFingerprint(_options.scenePath, _options.sceneHash, _world.SceneObjectCount(), _options.prefabs);
        _world.SetDestroyedHandler([this](const NetworkId id, const bool wasSpawned)
                                   { _pendingDespawns.emplace_back(id.value, wasSpawned); });
    }

    NetworkServer::~NetworkServer()
    {
        _world.SetDestroyedHandler({});
    }

    void NetworkServer::Update(const std::uint64_t nowMicros)
    {
        _now = nowMicros;

        if (!_worldLost && !_world.HasScene())
        {
            _worldLost = true;
            Logger::Error("NetworkServer: the scene the network world was built for is gone; every client session ends "
                          "(a scene change is not replicated yet)");
            for (auto &entry : _clients)
            {
                MarkDrop(entry.second, "the scene ended", nullptr);
            }
        }

        const auto handler = [this](const N2Net::TransportEvent &event) { HandleEvent(event); };
        _transport.Poll(handler);

        if (_worldLost)
        {
            _pendingDespawns.clear();
        }
        else
        {
            FlushPendingDespawns();
        }
        RunTimers();
        SendPackets();
        ApplyDrops();
    }

    void NetworkServer::HandleEvent(const N2Net::TransportEvent &event)
    {
        if (_transportObserver)
        {
            _transportObserver(event);
        }
        switch (event.type)
        {
        case N2Net::TransportEventType::PeerConnected:
            OnPeerConnected(event.peer);
            break;
        case N2Net::TransportEventType::PeerDisconnected:
            OnPeerDisconnected(event.peer);
            break;
        case N2Net::TransportEventType::Datagram:
            OnDatagram(event.peer, event.data);
            break;
        }
    }

    void NetworkServer::OnPeerConnected(const N2Net::PeerId peer)
    {
        if (_clients.contains(peer))
        {
            return;
        }
        if (_worldLost || _clients.size() >= _options.maxClients || _nextClientId == kServerClientId)
        {
            ++_stats.droppedOverCapacity;
            _refusedPeers.push_back(peer);
            return;
        }

        Client client;
        client.peer = peer;
        client.id = _nextClientId;
        ++_nextClientId;
        client.connection =
            std::make_unique<N2Net::Connection>(MakeWorldConnectionConfig(_options.protocolId, _transport), _now);
        client.connectedAt = _now;
        client.rateWindowStart = _now;
        const auto inserted = _clients.emplace(peer, std::move(client));

        ServerHelloMessage hello;
        hello.fingerprint = _fingerprint;
        hello.clientId = inserted.first->second.id;
        Send(inserted.first->second, Encode(hello));
    }

    void NetworkServer::OnPeerDisconnected(const N2Net::PeerId peer)
    {
        const auto found = _clients.find(peer);
        if (found == _clients.end())
        {
            return;
        }
        if (found->second.dropping)
        {
            // Already being dropped: ApplyDrops reports it
            return;
        }
        NotifyLeft(found->second, "disconnected");
        _clients.erase(found);
    }

    void NetworkServer::OnDatagram(const N2Net::PeerId peer, const std::span<const std::uint8_t> data)
    {
        const auto found = _clients.find(peer);
        if (found == _clients.end() || found->second.dropping)
        {
            return;
        }
        Client &client = found->second;
        const auto handler = [this, &client](const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
        { OnMessage(client, channel, message); };
        // A datagram the connection rejects is counted by it and changes nothing
        (void)client.connection->OnPacket(_now, data, handler);
    }

    void NetworkServer::OnMessage(Client &client, const N2Net::MessageChannel channel,
                                  const std::span<const std::uint8_t> message)
    {
        if (client.dropping)
        {
            return;
        }
        if (!AllowMessage(client))
        {
            MarkDrop(client, "it sent more messages per second than allowed", &NetworkServerStats::droppedRateLimited);
            return;
        }
        if (channel != N2Net::MessageChannel::ReliableOrdered)
        {
            MarkDrop(client, "it sent a message on the unreliable channel, which the world protocol does not use",
                     &NetworkServerStats::droppedProtocol);
            return;
        }

        const std::expected<ClientMessage, DecodeError> decoded = DecodeClientMessage(message);
        if (!decoded)
        {
            if (decoded.error() == DecodeError::WrongDirection)
            {
                MarkDrop(client,
                         "it sent a message only the server may send: a client can never spawn or despawn anything",
                         &NetworkServerStats::droppedUnauthorized);
            }
            else
            {
                MarkDrop(client, std::format("it sent a malformed message ({})", ToString(decoded.error())),
                         &NetworkServerStats::droppedMalformed);
            }
            return;
        }

        if (const auto *accept = std::get_if<ClientAcceptMessage>(&*decoded))
        {
            OnAccept(client, accept->fingerprint);
            return;
        }
        if (const auto *reject = std::get_if<ClientRejectMessage>(&*decoded))
        {
            MarkDrop(client, std::format("it refused to join ({})", ToString(reject->reason)),
                     &NetworkServerStats::clientsRefused);
        }
    }

    void NetworkServer::OnAccept(Client &client, const Fingerprint &fingerprint)
    {
        if (client.state != ClientState::Handshaking)
        {
            MarkDrop(client, "it sent a second accept", &NetworkServerStats::droppedProtocol);
            return;
        }
        const JoinRefusal mismatch = CompareFingerprints(_fingerprint, fingerprint);
        if (mismatch != JoinRefusal::None)
        {
            MarkDrop(client, std::format("it accepted with a fingerprint that differs from the server's ({})", ToString(mismatch)),
                     &NetworkServerStats::droppedBadFingerprint);
            return;
        }

        client.state = ClientState::Joined;
        ++_stats.clientsJoined;
        SendJoinState(client);
        if (!client.dropping && _clientObserver)
        {
            NetworkClientEvent event;
            event.type = NetworkClientEvent::Type::Joined;
            event.client = client.id;
            event.peer = client.peer;
            _clientObserver(event);
        }
    }

    void NetworkServer::SendJoinState(Client &client)
    {
        std::uint32_t destroyed = 0;
        for (const std::uint32_t id : _destroyedSceneIds)
        {
            DespawnMessage message;
            message.id = NetworkId{id};
            if (!Send(client, Encode(message)))
            {
                return;
            }
            ++destroyed;
        }

        std::uint32_t spawned = 0;
        for (const auto &entry : _world.Objects())
        {
            const NetworkObject &object = *entry.second;
            if (!object.IsSpawned())
            {
                continue;
            }
            const std::optional<std::uint16_t> index = _options.prefabs.IndexOf(object.GetPrefabName());
            if (!index.has_value())
            {
                // Only prefabs of the table are ever spawned, so this cannot happen
                continue;
            }
            SpawnMessage message;
            message.id = object.GetNetworkId();
            message.prefabIndex = *index;
            message.owner = object.GetOwner();
            if (!Send(client, Encode(message)))
            {
                return;
            }
            ++spawned;
        }

        WorldReadyMessage ready;
        ready.spawned = spawned;
        ready.destroyedSceneObjects = destroyed;
        Send(client, Encode(ready));
    }

    bool NetworkServer::Send(Client &client, const std::vector<std::uint8_t> &message)
    {
        if (client.dropping)
        {
            return false;
        }
        if (client.connection->SendReliable(message) == N2Net::SendResult::Ok)
        {
            return true;
        }
        MarkDrop(client, "its reliable send queue is full or its connection failed", &NetworkServerStats::droppedSendFailed);
        return false;
    }

    void NetworkServer::Broadcast(const std::vector<std::uint8_t> &message)
    {
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            if (client.state == ClientState::Joined && !client.dropping)
            {
                Send(client, message);
            }
        }
    }

    std::expected<NetworkId, std::string> NetworkServer::Spawn(const std::string_view prefabName, const ClientId owner)
    {
        if (!_world.HasScene())
        {
            return std::unexpected(std::string("the network world has no scene"));
        }
        const std::optional<std::uint16_t> index = _options.prefabs.IndexOf(prefabName);
        if (!index.has_value())
        {
            return std::unexpected(std::format("prefab '{}' is not in the prefab table this session started with",
                                               SanitizeText(prefabName, 64)));
        }
        if (owner != kServerClientId && !IsClientJoined(owner))
        {
            return std::unexpected(std::format("client {} has not joined this server, so it can't own an object", owner));
        }
        const NetworkId id = _world.AllocateId();
        if (!id.IsValid())
        {
            return std::unexpected(std::string("the net id counter is exhausted: no more objects can be spawned in this "
                                               "session"));
        }
        const std::expected<NetworkObject *, std::string> object = _world.SpawnInstance(id, prefabName, owner);
        if (!object)
        {
            return std::unexpected(object.error());
        }

        SpawnMessage message;
        message.id = id;
        message.prefabIndex = *index;
        message.owner = owner;
        Broadcast(Encode(message));
        ++_stats.spawnsSent;
        return id;
    }

    bool NetworkServer::Despawn(const NetworkId id)
    {
        const NetworkWorld::DespawnResult result = _world.Despawn(id);
        if (!result.found)
        {
            return false;
        }
        if (!result.wasSpawned)
        {
            _destroyedSceneIds.insert(id.value);
        }
        DespawnMessage message;
        message.id = id;
        Broadcast(Encode(message));
        ++_stats.despawnsSent;
        return true;
    }

    void NetworkServer::DropPeer(const N2Net::PeerId peer, const std::string_view reason)
    {
        const auto found = _clients.find(peer);
        if (found == _clients.end())
        {
            _transport.Disconnect(peer);
            return;
        }
        MarkDrop(found->second, std::string(reason), nullptr);
        ApplyDrops();
    }

    std::size_t NetworkServer::JoinedClientCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto &entry : _clients)
        {
            if (entry.second.state == ClientState::Joined && !entry.second.dropping)
            {
                ++count;
            }
        }
        return count;
    }

    bool NetworkServer::IsClientJoined(const ClientId client) const
    {
        for (const auto &entry : _clients)
        {
            if (entry.second.id == client)
            {
                return entry.second.state == ClientState::Joined && !entry.second.dropping;
            }
        }
        return false;
    }

    std::vector<ClientId> NetworkServer::JoinedClients() const
    {
        std::vector<ClientId> ids;
        for (const auto &entry : _clients)
        {
            if (entry.second.state == ClientState::Joined && !entry.second.dropping)
            {
                ids.push_back(entry.second.id);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    void NetworkServer::FlushPendingDespawns()
    {
        std::vector<std::pair<std::uint32_t, bool>> pending;
        pending.swap(_pendingDespawns);
        for (const auto &entry : pending)
        {
            if (!entry.second)
            {
                _destroyedSceneIds.insert(entry.first);
            }
            DespawnMessage message;
            message.id = NetworkId{entry.first};
            Broadcast(Encode(message));
            ++_stats.despawnsSent;
        }
    }

    void NetworkServer::RunTimers()
    {
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            if (client.dropping)
            {
                continue;
            }
            if (client.state == ClientState::Handshaking && _now - client.connectedAt > _options.joinTimeoutMicros)
            {
                MarkDrop(client, "it did not accept the world in time", &NetworkServerStats::droppedJoinTimeout);
                continue;
            }
            if (client.connection->IsFailed())
            {
                MarkDrop(client, std::format("its connection failed ({})", FailureName(client.connection->Failure())),
                         &NetworkServerStats::droppedConnectionFailed);
            }
        }
    }

    void NetworkServer::SendPackets()
    {
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            // A client being dropped still gets what its connection owes it (the acknowledgement of a refusal, for one):
            // nothing new is queued for it, the messages are only the connection's own. Not one whose connection failed.
            if (client.connection->IsFailed())
            {
                continue;
            }
            const N2Net::PeerId peer = client.peer;
            const auto sender = [this, peer](const std::span<const std::uint8_t> packet) { _transport.Send(peer, packet); };
            client.connection->Update(_now, sender);
        }
    }

    void NetworkServer::ApplyDrops()
    {
        for (const N2Net::PeerId peer : _refusedPeers)
        {
            _transport.Disconnect(peer);
        }
        _refusedPeers.clear();

        for (auto it = _clients.begin(); it != _clients.end();)
        {
            Client &client = it->second;
            if (!client.dropping)
            {
                ++it;
                continue;
            }
            NotifyLeft(client, client.dropReason);
            _transport.Disconnect(client.peer);
            it = _clients.erase(it);
        }
    }

    void NetworkServer::MarkDrop(Client &client, std::string reason, std::uint64_t NetworkServerStats::*const counter)
    {
        if (client.dropping)
        {
            return;
        }
        client.dropping = true;
        client.dropReason = std::move(reason);
        if (counter != nullptr)
        {
            ++(_stats.*counter);
        }
        const bool refused = counter == &NetworkServerStats::clientsRefused;
        if (!refused)
        {
            ++_stats.clientsDropped;
        }
        // The reasons are fixed texts of this file (nothing a client sent is copied into a log line)
        const std::string line = std::format("Network client {} (peer {}) is dropped: {}", client.id, client.peer,
                                             client.dropReason);
        if (refused)
        {
            Logger::Info(line, Logger::LogSensitivity::Admin);
        }
        else
        {
            Logger::Warn(line, Logger::LogSensitivity::Admin);
        }
    }

    bool NetworkServer::AllowMessage(Client &client)
    {
        if (_now - client.rateWindowStart >= kMicrosPerSecond)
        {
            client.rateWindowStart = _now;
            client.rateWindowCount = 0;
        }
        ++client.rateWindowCount;
        return client.rateWindowCount <= _options.maxClientMessagesPerSecond;
    }

    void NetworkServer::NotifyLeft(const Client &client, const std::string_view reason)
    {
        if (!_clientObserver)
        {
            return;
        }
        NetworkClientEvent event;
        event.type = NetworkClientEvent::Type::Left;
        event.client = client.id;
        event.peer = client.peer;
        event.wasJoined = client.state == ClientState::Joined;
        event.reason = std::string(reason);
        _clientObserver(event);
    }
}
