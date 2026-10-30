#include "engine/network/NetworkServer.hpp"

#include <algorithm>
#include <format>
#include <optional>

#include "engine/Logger.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Time.hpp"
#include "engine/network/LagCompensatedHitbox.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkProtocol.hpp"

namespace N2Engine::Network
{
    namespace
    {
        constexpr std::uint64_t kMicrosPerSecond = 1'000'000;
        constexpr std::uint32_t kHardMaxClients = 1024;
        constexpr std::uint32_t kMaxStateSendRateHz = 240;
        NetworkServer *g_physicsQueryServer = nullptr;

        /// The server most recently constructed that is still alive (NetworkServer::Active)
        NetworkServer *g_activeServer = nullptr;

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
        : _transport(transport), _world(world), _options(std::move(options)), _replicator(_options.replication),
          _lagCompensation(_options.lagCompMaxMicros,256)
    {
        _options.maxClients = std::clamp<std::uint32_t>(_options.maxClients, 1u, kHardMaxClients);
        _options.stateSendRateHz = std::min<std::uint32_t>(_options.stateSendRateHz, kMaxStateSendRateHz);
        _fingerprint = MakeFingerprint(_options.scenePath, _options.sceneHash, _world.SceneObjectCount(), _options.prefabs,
                                       _options.inputLayout);
        _options.input = _options.input.Clamped();
        _options.rpc = _options.rpc.Clamped();
        _options.tickRate = std::clamp<std::uint32_t>(_options.tickRate, 1u, kMaxClockTickRate);
        g_activeServer = this;
        _world.SetDestroyedHandler([this](const NetworkId id, const bool wasSpawned)
                                   { _pendingDespawns.emplace_back(id.value, wasSpawned); });
    }

    NetworkServer::~NetworkServer()
    {
        if (g_physicsQueryServer == this) g_physicsQueryServer = nullptr;
        if (g_activeServer == this)
        {
            g_activeServer = nullptr;
        }
        _world.SetDestroyedHandler({});
    }

    void NetworkServer::BindPhysicsQueries() { g_physicsQueryServer = this; }
    bool NetworkServer::SetObserver(ClientId client, NetworkId observer)
    {
        const auto *object = _world.Find(observer);
        if (!IsClientJoined(client) || (observer.IsValid() && (!object || object->GetOwner() != client))) return false;
        return _replicator.SetObserver(client,observer);
    }
    NetworkServer *NetworkServer::PhysicsQueryServer() noexcept { return g_physicsQueryServer; }

    bool NetworkServer::CaptureHitboxes(std::uint64_t now, std::span<const HistoricalHitbox> boxes)
    {
        if (!_world.HasScene()) return false;
        for (const auto &box : boxes) if (!_world.Find(box.object)) return false;
        return _lagCompensation.Capture(now, boxes);
    }

    std::optional<LagCompensatedHit> NetworkServer::CastLagCompensated(ClientId requester,
        std::uint64_t renderTime, const Math::Vector3 &origin, const Math::Vector3 &direction,
        float maxDistance, float radius, std::uint32_t layers, NetworkId ignore) const
    {
        if (!_world.HasScene()) return {};
        for (const auto &[peer, client] : _clients)
        {
            if (client.id != requester || client.state != ClientState::Joined || client.dropping ||
                !client.connection->Rtt().HasSample()) continue;
            return _lagCompensation.Cast(_now, renderTime, client.connection->Rtt().SmoothedMicros(),
                origin, direction, maxDistance, radius, layers, ignore);
        }
        return {};
    }

    void NetworkServer::Update(const std::uint64_t nowMicros)
    {
        _now = nowMicros;
        if (!_haveSeenTick || _seenTick != _inputTick)
        {
            // The host moved to another tick since the last update: the part of a tick that has passed counts from here
            _haveSeenTick = true;
            _seenTick = _inputTick;
            _seenTickAt = _now;
        }

        if (_options.autoCaptureHitboxes && _world.HasScene() && _lastHitboxTick != Time::GetTick())
        {
            _lastHitboxTick = Time::GetTick();
            std::vector<HistoricalHitbox> boxes;
            for (const auto &[id,object] : _world.Objects())
                if (const auto *hitbox = object->GetGameObject().GetComponent<LagCompensatedHitbox>())
                    if (const auto box = hitbox->Snapshot(NetworkId{id})) boxes.push_back(*box);
            (void)_lagCompensation.Capture(_now,boxes);
        }

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
            DispatchRpcs();
            FlushPendingDespawns();
            SendState();
            SendInputAcks();
            SendClockPongs();
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
            _refusedPeers.emplace_back(peer, _worldLost ? "refused: the world is lost"
                                             : _nextClientId == kServerClientId ? "refused: no client id is left"
                                                                                 : "refused: the server is full");
            return;
        }

        Client client;
        client.peer = peer;
        client.id = _nextClientId;
        ++_nextClientId;
        // A multiplexer may contain both datagram and stream peers.
        auto connectionOptions = MakeWorldConnectionConfig(_options.protocolId, _transport);
        connectionOptions.reliableStream = _transport.IsReliablePeer(peer);
        connectionOptions.maxPacketSize = static_cast<std::uint32_t>(
            std::min(_transport.MaxPeerPacketSize(peer),N2Net::kMaxPacketSize));
        client.connection = std::make_unique<N2Net::Connection>(connectionOptions, _now);
        client.connectedAt = _now;
        client.rateWindowStart = _now;
        client.ackWindowStart = _now;
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
        RetireInput(found->second);
        _replicator.RemoveClient(found->second.id);
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
        if (channel == N2Net::MessageChannel::UnreliableSequenced)
        {
            OnUnreliable(client, message);
            return;
        }
        if (!message.empty() && message[0] == kRpcClientReliableType)
        {
            // RPCs have budgets of their own (RpcLimits); the world-message ceiling below is for the accept and the reject
            OnRpc(client, message);
            return;
        }
        if (!AllowMessage(client))
        {
            MarkDrop(client, "it sent more messages per second than allowed", &NetworkServerStats::droppedRateLimited);
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

    void NetworkServer::OnUnreliable(Client &client, const std::span<const std::uint8_t> message)
    {
        if (!message.empty() && message[0] == kInputFramesType)
        {
            OnInputFrames(client, message);
            return;
        }
        if (!message.empty() && message[0] == kClockPingType)
        {
            OnClockPing(client, message);
            return;
        }
        if (!message.empty() && message[0] == kRpcClientUnreliableType)
        {
            OnRpc(client, message);
            return;
        }
        if (!AllowAck(client))
        {
            MarkDrop(client, "it sent more state acknowledgements per second than allowed",
                     &NetworkServerStats::droppedRateLimited);
            return;
        }
        // The one thing a client sends on the unreliable channel is the acknowledgement of state it applied. It carries
        // sequence numbers and nothing else: no value of any field.
        if (message.empty() || message[0] != kStateAckType)
        {
            MarkDrop(client, "it sent a message on the unreliable channel that is not a state acknowledgement",
                     &NetworkServerStats::droppedProtocol);
            return;
        }
        const std::expected<StateAckMessage, DecodeError> ack = DecodeStateAck(message);
        if (!ack)
        {
            MarkDrop(client, std::format("it sent a malformed state acknowledgement ({})", ToString(ack.error())),
                     &NetworkServerStats::droppedMalformed);
            return;
        }
        if (client.state != ClientState::Joined)
        {
            MarkDrop(client, "it acknowledged state before it joined", &NetworkServerStats::droppedProtocol);
            return;
        }
        if (!_replicator.OnAck(client.id, *ack))
        {
            MarkDrop(client, "it acknowledged a state update the server never sent", &NetworkServerStats::droppedProtocol);
        }
    }

    void NetworkServer::OnRpc(Client &client, const std::span<const std::uint8_t> message)
    {
        if (client.state != ClientState::Joined)
        {
            MarkDrop(client, "it sent an RPC before it joined", &NetworkServerStats::droppedProtocol);
            return;
        }
        RpcHeader header;
        if (!DecodeRpcHeader(message, header))
        {
            MarkDrop(client, "it sent a malformed RPC message", &NetworkServerStats::droppedMalformed);
            return;
        }
        ++_rpcStats.received;
        const auto refuse = [this, &client](std::uint64_t RpcStats::*counter, const char *why)
        {
            ++(_rpcStats.*counter);
            if (_rpcLog.Allow())
            {
                Logger::Warn(std::format("NetworkServer: RPC from client {} refused: {}", client.id, why));
            }
        };

        const RpcBudget::Verdict verdict = client.rpcBudget.Admit(
            _now, RpcBudgetKey(header.componentIndex, header.rpcIndex), message.size(), _options.rpc);
        if (verdict != RpcBudget::Verdict::Allowed)
        {
            refuse(&RpcStats::droppedRate,
                   verdict == RpcBudget::Verdict::OverRate ? "over its RPC rate budget" : "over its RPC byte budget");
            return;
        }
        if (client.pendingRpcs.size() >= _options.rpc.maxQueued)
        {
            refuse(&RpcStats::droppedQueueFull, "its RPC queue is full");
            return;
        }

        NetworkObject *const networked = _world.Find(header.object);
        if (networked == nullptr)
        {
            refuse(&RpcStats::rejectedUnknown, "no such object");
            return;
        }
        // Authority: a client calls RPCs only on the objects it owns (the objects of the server have owner 0, never a client)
        if (networked->GetOwner() != client.id || client.id == kServerClientId)
        {
            refuse(&RpcStats::rejectedAuthority, "the client does not own the object");
            return;
        }
        const auto &components = networked->GetGameObject().GetAllComponents();
        if (header.componentIndex >= components.size() || components[header.componentIndex] == nullptr)
        {
            refuse(&RpcStats::rejectedUnknown, "no such component");
            return;
        }
        const std::vector<RpcDescriptor> *const table = RpcTableOf(*components[header.componentIndex]);
        if (table == nullptr || header.rpcIndex >= table->size() || HashRpcTable(*table) != header.tableHash)
        {
            refuse(&RpcStats::rejectedUnknown, "no such RPC (or the RPC tables differ)");
            return;
        }
        const RpcDescriptor &descriptor = (*table)[header.rpcIndex];
        if (descriptor.direction != RpcDirection::ClientToServer)
        {
            refuse(&RpcStats::rejectedDirection, "the RPC is not a client-to-server RPC");
            return;
        }
        if (descriptor.reliable != (header.type == kRpcClientReliableType))
        {
            refuse(&RpcStats::rejectedUnknown, "the RPC arrived on the other channel than it is declared for");
            return;
        }
        if (_options.cosmeticRpcsOnly && !descriptor.cosmetic)
        {
            refuse(&RpcStats::rejectedSimulation, "the RPC changes the simulation, which this preset does not allow");
            return;
        }
        PendingRpc call;
        call.sender = client.id;
        call.object = header.object;
        call.componentIndex = header.componentIndex;
        call.rpcIndex = header.rpcIndex;
        call.tableHash = header.tableHash;
        if (!DecodeRpcArgs(descriptor, header.args, call.args))
        {
            refuse(&RpcStats::rejectedArguments, "the arguments do not decode or are not valid");
            return;
        }
        client.pendingRpcs.push_back(std::move(call));
    }

    void NetworkServer::DispatchRpcs()
    {
        std::vector<std::pair<ClientId, std::vector<PendingRpc>>> batches;
        for (auto &entry : _clients)
        {
            if (!entry.second.pendingRpcs.empty())
            {
                batches.emplace_back(entry.second.id, std::move(entry.second.pendingRpcs));
                entry.second.pendingRpcs.clear();
            }
        }
        // Clients in id order, each client's calls in the order they arrived: the same on every run
        std::sort(batches.begin(), batches.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &batch : batches)
        {
            for (const PendingRpc &call : batch.second)
            {
                RunPendingRpc(_world, call, true, _rpcStats);
            }
        }
    }

    RpcSendResult NetworkServer::CallRpc(Component &component, const std::string_view name,
                                         const std::vector<nlohmann::json> &args, const RpcTarget target,
                                         const ClientId clientId)
    {
        NetworkObject *const networked = component.GetGameObject().GetComponent<NetworkObject>();
        if (networked == nullptr || !networked->HasNetworkId() || !networked->IsRegistered() ||
            _world.Find(networked->GetNetworkId()) != networked)
        {
            return RpcSendResult::UnknownObject;
        }
        ResolvedRpc rpc;
        if (!FindRpc(component.GetGameObject(), component, name, rpc))
        {
            return RpcSendResult::UnknownRpc;
        }
        const RpcDescriptor &descriptor = *rpc.descriptor;
        if (descriptor.direction == RpcDirection::ClientToServer)
        {
            return RpcSendResult::WrongDirection;
        }
        if (_options.cosmeticRpcsOnly && !descriptor.cosmetic)
        {
            ++_rpcStats.rejectedSimulation;
            return RpcSendResult::NotSimulationSafe;
        }
        if (descriptor.direction == RpcDirection::ServerToOwner && target != RpcTarget::Owner)
        {
            return RpcSendResult::WrongDirection;
        }

        std::vector<Client *> targets;
        const NetworkId id = networked->GetNetworkId();
        if (target == RpcTarget::All)
        {
            for (auto &entry : _clients)
            {
                Client &candidate = entry.second;
                if (candidate.state == ClientState::Joined && !candidate.dropping && !candidate.hidden.contains(id.value))
                {
                    targets.push_back(&candidate);
                }
            }
        }
        else
        {
            const ClientId wanted = target == RpcTarget::Owner ? networked->GetOwner() : clientId;
            if (wanted != kServerClientId)
            {
                for (auto &entry : _clients)
                {
                    if (entry.second.id == wanted && entry.second.state == ClientState::Joined && !entry.second.dropping)
                    {
                        targets.push_back(&entry.second);
                    }
                }
            }
        }
        if (targets.empty())
        {
            return RpcSendResult::NoTarget;
        }

        const std::vector<RpcDescriptor> *const table = RpcTableOf(component);
        std::string error;
        const std::optional<std::vector<std::uint8_t>> message =
            EncodeRpcMessage(descriptor.reliable ? kRpcServerReliableType : kRpcServerUnreliableType, id, rpc,
                             HashRpcTable(*table), args, error);
        if (!message.has_value())
        {
            if (_rpcLog.Allow())
            {
                Logger::Warn(std::format("NetworkServer::CallRpc: {}", error));
            }
            return RpcSendResult::BadArguments;
        }

        std::size_t sent = 0;
        for (Client *const receiver : targets)
        {
            if (!receiver->rpcBudget.AdmitSend(_now, _options.rpc))
            {
                ++_rpcStats.droppedSendBudget;
                continue;
            }
            bool ok = false;
            if (descriptor.reliable)
            {
                ok = Send(*receiver, *message);
            }
            else
            {
                ok = message->size() <= receiver->connection->MaxUnreliableMessageSize() &&
                     receiver->connection->SendUnreliable(*message) == N2Net::SendResult::Ok;
            }
            if (ok)
            {
                ++sent;
                ++_rpcStats.sent;
            }
            else
            {
                ++_rpcStats.sendFailed;
            }
        }
        return sent > 0 ? RpcSendResult::Sent : RpcSendResult::OverBudget;
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
        _replicator.AddClient(client.id);
        if (!_options.inputLayout.Empty())
        {
            client.input = std::make_unique<ServerInputBuffer>(_options.inputLayout, _options.input, _inputTick);
        }
        client.inputWindowStart = _now;
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
        // Clients must see the despawns of objects the game already destroyed before this spawn (the capacity of the
        // world counts them as gone)
        FlushPendingDespawns();
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
        _replicator.ForgetObject(id);
        _lagCompensation.Forget(id);
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
            _replicator.ForgetObject(NetworkId{entry.first});
            _lagCompensation.Forget(NetworkId{entry.first});
            DespawnMessage message;
            message.id = NetworkId{entry.first};
            Broadcast(Encode(message));
            ++_stats.despawnsSent;
        }
    }

    void NetworkServer::OnInputFrames(Client &client, const std::span<const std::uint8_t> message)
    {
        if (!AllowInput(client))
        {
            MarkDrop(client, "it sent more input messages per second than allowed", &NetworkServerStats::droppedRateLimited);
            return;
        }
        if (client.state != ClientState::Joined || client.input == nullptr)
        {
            MarkDrop(client, "it sent input before it joined, or to a server that takes no input",
                     &NetworkServerStats::droppedProtocol);
            return;
        }
        // The length of a frame is the layout's: nothing the message says decides what is allocated
        const std::expected<std::vector<InputFrame>, DecodeError> frames =
            DecodeInputFrames(message, _options.inputLayout.PayloadBytes());
        if (!frames)
        {
            MarkDrop(client, std::format("it sent a malformed input message ({})", ToString(frames.error())),
                     &NetworkServerStats::droppedMalformed);
            return;
        }
        for (const InputFrame &frame : *frames)
        {
            if (client.input->Receive(frame) == InputReceive::Invalid)
            {
                MarkDrop(client, "it sent an input frame that is not a value its layout produces",
                         &NetworkServerStats::droppedMalformed);
                return;
            }
        }
    }

    void NetworkServer::SendInputAcks()
    {
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            if (client.input == nullptr || client.state != ClientState::Joined || client.dropping ||
                client.connection->IsFailed() || !client.input->AckPending())
            {
                continue;
            }
            // A lost ack is as good as none: the next input message brings another
            (void)client.connection->SendUnreliable(Encode(client.input->TakeAck()));
        }
    }

    void NetworkServer::OnClockPing(Client &client, const std::span<const std::uint8_t> message)
    {
        if (!AllowClockPing(client))
        {
            MarkDrop(client, "it sent more clock pings per second than allowed", &NetworkServerStats::droppedRateLimited);
            return;
        }
        if (client.state != ClientState::Joined)
        {
            MarkDrop(client, "it sent a clock ping before it joined", &NetworkServerStats::droppedProtocol);
            return;
        }
        const std::expected<ClockPingMessage, DecodeError> ping = DecodeClockPing(message);
        if (!ping)
        {
            MarkDrop(client, std::format("it sent a malformed clock ping ({})", ToString(ping.error())),
                     &NetworkServerStats::droppedMalformed);
            return;
        }
        // The answer is stamped now and sent by the next SendClockPongs (never from inside the connection's receive).
        // The pings of one update beyond the cap get no answer: the client asks again.
        constexpr std::size_t kMaxPendingPongs = 8;
        if (client.pendingPongs.size() >= kMaxPendingPongs)
        {
            return;
        }
        const std::uint32_t microsPerTick = 1'000'000u / _options.tickRate;
        const std::uint64_t sinceTick = _now > _seenTickAt ? _now - _seenTickAt : 0;
        ClockPongMessage pong;
        pong.sequence = ping->sequence;
        pong.tick = _inputTick;
        pong.tickMicros = static_cast<std::uint32_t>(std::min<std::uint64_t>(sinceTick, microsPerTick));
        pong.tickRate = static_cast<std::uint16_t>(_options.tickRate);
        client.pendingPongs.push_back(pong);
    }

    void NetworkServer::SendClockPongs()
    {
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            if (client.pendingPongs.empty())
            {
                continue;
            }
            if (client.state == ClientState::Joined && !client.dropping && !client.connection->IsFailed())
            {
                for (const ClockPongMessage &pong : client.pendingPongs)
                {
                    // A lost pong is as good as none: the client pings again
                    if (client.connection->SendUnreliable(Encode(pong)) == N2Net::SendResult::Ok)
                    {
                        ++_stats.clockPongsSent;
                    }
                }
            }
            client.pendingPongs.clear();
        }
    }

    bool NetworkServer::AllowClockPing(Client &client)
    {
        if (_now - client.clockWindowStart >= kMicrosPerSecond)
        {
            client.clockWindowStart = _now;
            client.clockWindowCount = 0;
        }
        ++client.clockWindowCount;
        return client.clockWindowCount <= _options.maxClockPingsPerSecond;
    }

    void NetworkServer::RetireInput(Client &client)
    {
        if (client.input != nullptr)
        {
            _departedInput.Add(client.input->Stats());
            client.input.reset();
        }
    }

    void NetworkServer::SetCurrentTick(const std::uint32_t tick)
    {
        _inputTick = tick;
        for (auto &entry : _clients)
        {
            if (entry.second.input != nullptr)
            {
                entry.second.input->SetNow(tick);
            }
        }
    }

    std::optional<ControlledInput> NetworkServer::InputFor(const NetworkId id, const std::uint32_t tick)
    {
        if (_worldLost)
        {
            return std::nullopt;
        }
        const NetworkObject *const object = _world.Find(id);
        if (object == nullptr)
        {
            return std::nullopt;
        }
        // Authority: the input is the owner's and nobody else's. The server's own objects have no input.
        const ClientId owner = object->GetOwner();
        if (owner == kServerClientId)
        {
            return std::nullopt;
        }
        return InputForClient(owner, tick);
    }

    std::optional<ControlledInput> NetworkServer::InputForClient(const ClientId clientId, const std::uint32_t tick)
    {
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            if (client.id != clientId)
            {
                continue;
            }
            if (client.state != ClientState::Joined || client.dropping || client.input == nullptr)
            {
                return std::nullopt;
            }
            const std::optional<ResolvedInput> resolved = client.input->Resolve(tick);
            if (!resolved.has_value())
            {
                return std::nullopt;
            }
            std::optional<std::vector<float>> values = _options.inputLayout.Unpack(resolved->payload);
            if (!values.has_value())
            {
                // Cannot happen: only payloads the layout produces are ever buffered
                return std::nullopt;
            }
            ControlledInput input;
            input.client = clientId;
            input.tick = resolved->tick;
            input.source = resolved->source;
            input.frameTick = resolved->frameTick;
            input.hasFrameTick = resolved->hasFrameTick;
            if (resolved->hasFrameTick &&
                (!client.haveAppliedFrame || TickDifference(resolved->tick, client.appliedOnTick) >= 0))
            {
                // The newest tick asked for wins; asking again for an older one changes nothing
                client.haveAppliedFrame = true;
                client.appliedFrameTick = resolved->frameTick;
                client.appliedOnTick = resolved->tick;
            }
            input.values = std::move(*values);
            return input;
        }
        return std::nullopt;
    }

    ServerInputStats NetworkServer::InputStats() const
    {
        ServerInputStats total = _departedInput;
        for (const auto &entry : _clients)
        {
            if (entry.second.input != nullptr)
            {
                total.Add(entry.second.input->Stats());
            }
        }
        return total;
    }

    std::optional<ServerInputStats> NetworkServer::InputStatsFor(const ClientId clientId) const
    {
        for (const auto &entry : _clients)
        {
            if (entry.second.id == clientId && entry.second.input != nullptr)
            {
                return entry.second.input->Stats();
            }
        }
        return std::nullopt;
    }

    NetworkServer *NetworkServer::Active() noexcept
    {
        return g_activeServer;
    }

    void NetworkServer::SendState()
    {
        if (_options.stateSendRateHz == 0 || _worldLost || _now < _nextStateAt)
        {
            return;
        }
        _nextStateAt = _now + kMicrosPerSecond / _options.stateSendRateHz;
        if (JoinedClientCount() == 0)
        {
            return;
        }
        _replicator.BeginTick(_world);
        for (auto &entry : _clients)
        {
            Client &client = entry.second;
            if (client.state != ClientState::Joined || client.dropping || client.connection->IsFailed())
            {
                continue;
            }
            const auto visible = _replicator.VisibleObjects(client.id);
            std::erase_if(client.hidden,[&](auto id){ return !_world.Find(NetworkId{id}); });
            for (const auto &[id,object] : _world.Objects())
            {
                const bool shown = visible.contains(id);
                const bool wasShown = !client.hidden.contains(id);
                if (shown == wasShown) continue;
                if (!Send(client,Encode(VisibilityMessage{NetworkId{id},shown,object->GetGameObject().IsActive()}))) break;
                if (shown) client.hidden.erase(id); else client.hidden.insert(id);
            }
            const std::vector<std::vector<std::uint8_t>> messages =
                _replicator.BuildForClient(client.id, _now, client.connection->MaxUnreliableMessageSize(), _inputTick,
                                           client.haveAppliedFrame ? std::optional<std::uint32_t>(client.appliedFrameTick)
                                                                   : std::nullopt);
            for (const std::vector<std::uint8_t> &message : messages)
            {
                // A message that does not get out is as good as a lost one: the next tick sends what is still needed
                (void)client.connection->SendUnreliable(message);
            }
            if (messages.empty())
            {
                // Nothing changed for this client: say how far the stream has got, so it can tell objects that stopped
                // from snapshots that are late (netcode stage B3)
                (void)client.connection->SendUnreliable(Encode(StreamTickMessage{_inputTick}));
            }
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
        // A refused peer was never a client, but whoever watches the transport saw it connect (N2Server lists every
        // session), and the local Disconnect below raises no event: the client observer is told it left
        std::vector<std::pair<N2Net::PeerId, std::string>> refused;
        refused.swap(_refusedPeers);
        for (const auto &[peer, reason] : refused)
        {
            if (_clientObserver)
            {
                NetworkClientEvent event;
                event.type = NetworkClientEvent::Type::Left;
                event.client = kServerClientId;
                event.peer = peer;
                event.wasJoined = false;
                event.reason = reason;
                _clientObserver(event);
            }
            _transport.Disconnect(peer);
        }

        for (auto it = _clients.begin(); it != _clients.end();)
        {
            Client &client = it->second;
            if (!client.dropping)
            {
                ++it;
                continue;
            }
            NotifyLeft(client, client.dropReason);
            RetireInput(client);
            _replicator.RemoveClient(client.id);
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

    bool NetworkServer::AllowInput(Client &client)
    {
        if (_now - client.inputWindowStart >= kMicrosPerSecond)
        {
            client.inputWindowStart = _now;
            client.inputWindowCount = 0;
        }
        ++client.inputWindowCount;
        return client.inputWindowCount <= _options.input.maxMessagesPerSecond;
    }

    bool NetworkServer::AllowAck(Client &client)
    {
        if (_now - client.ackWindowStart >= kMicrosPerSecond)
        {
            client.ackWindowStart = _now;
            client.ackWindowCount = 0;
        }
        ++client.ackWindowCount;
        return client.ackWindowCount <= _options.maxClientAcksPerSecond;
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
