#include "engine/network/NetworkClient.hpp"

#include <algorithm>
#include <format>

#include "engine/Logger.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkTransform.hpp"

namespace N2Engine::Network
{
    namespace
    {
        /// The least time between two clock pings, whatever the options say
        constexpr std::uint64_t kMinClockIntervalMicros = 20'000;
        /// Samples the estimate wants before the client slows from the burst interval to the steady one
        constexpr std::size_t kClockBurstSamples = 8;
        constexpr double kMicrosPerSecond = 1'000'000.0;

        /// The client most recently constructed that is still alive (NetworkClient::Active)
        NetworkClient *g_activeClient = nullptr;
    }

    std::string_view ToString(const NetworkClientState state) noexcept
    {
        switch (state)
        {
        case NetworkClientState::Connecting:
            return "connecting";
        case NetworkClientState::AwaitingHello:
            return "awaiting-hello";
        case NetworkClientState::Joining:
            return "joining";
        case NetworkClientState::Joined:
            return "joined";
        case NetworkClientState::Refused:
            return "refused";
        case NetworkClientState::Closed:
            return "closed";
        case NetworkClientState::Failed:
            return "failed";
        }
        return "unknown";
    }

    NetworkClient::NetworkClient(N2Net::ITransport &transport, NetworkWorld &world, NetworkClientOptions options)
        : _transport(transport), _world(world), _options(std::move(options)),
          _input(_options.inputLayout, _options.inputRedundancy), _clock(_options.clock),
          _interpolator(_options.interpolation)
    {
        _localFingerprint = MakeFingerprint(_options.scenePath, _options.sceneHash, _world.SceneObjectCount(),
                                            _options.prefabs, _options.inputLayout);
        _options.clockBurstIntervalMicros = std::max<std::uint64_t>(_options.clockBurstIntervalMicros, kMinClockIntervalMicros);
        _options.clockIntervalMicros = std::max<std::uint64_t>(_options.clockIntervalMicros, kMinClockIntervalMicros);
        _options.rpc = _options.rpc.Clamped();
        _replicator.SetMessageObserver([this](const std::uint32_t tick) { OnStateMessage(tick); });
        _replicator.SetEntryObserver([this](NetworkObject &object, const std::uint32_t tick) { OnEntryApplied(object, tick); });
        g_activeClient = this;
    }

    NetworkClient::~NetworkClient()
    {
        if (g_activeClient == this)
        {
            g_activeClient = nullptr;
        }
    }

    NetworkClient *NetworkClient::Active() noexcept
    {
        return g_activeClient;
    }

    bool NetworkClient::Live() const noexcept
    {
        return _state == NetworkClientState::AwaitingHello || _state == NetworkClientState::Joining ||
               _state == NetworkClientState::Joined;
    }

    void NetworkClient::Update(const std::uint64_t nowMicros)
    {
        _now = nowMicros;
        if (_finished)
        {
            return;
        }

        const auto handler = [this](const N2Net::TransportEvent &event) { HandleEvent(event); };
        _transport.Poll(handler);
        DispatchRpcs();

        if (_leaveRequested && (Live() || _state == NetworkClientState::Connecting))
        {
            _state = NetworkClientState::Closed;
            _error = "this client left the server";
            _disconnectPending = true;
            Notify(NetworkSessionEvent{.type = NetworkSessionEvent::Type::Closed, .text = _error});
        }

        if (_state == NetworkClientState::AwaitingHello && _now - _helloWaitSince > _options.helloTimeoutMicros)
        {
            Fail("the server sent no hello in time");
        }
        if (_connection != nullptr && Live() && _connection->IsFailed())
        {
            Fail("the connection to the server failed (it timed out, or the server broke the protocol)");
        }

        // The acknowledgement of the state applied this update (one per update, however many messages arrived)
        if (_connection != nullptr && _state == NetworkClientState::Joined && !_disconnectPending &&
            _replicator.AckPending())
        {
            (void)_connection->SendUnreliable(Encode(_replicator.TakeAck()));
        }

        // The input frames queued since the last update, with the unacknowledged ones before them
        if (_connection != nullptr && _state == NetworkClientState::Joined && !_disconnectPending && _input.HasPending())
        {
            const std::optional<std::vector<std::uint8_t>> inputMessage =
                _input.BuildMessage(_connection->MaxUnreliableMessageSize());
            if (inputMessage.has_value())
            {
                // A message that does not get out is as good as a lost one: the next one repeats its frames
                (void)_connection->SendUnreliable(*inputMessage);
            }
        }

        // The clock ping: one at most per update, on a timer, so a long stall never sends a burst
        if (_connection != nullptr && _state == NetworkClientState::Joined && !_disconnectPending && _now >= _nextPingAt)
        {
            SendClockPing();
            _nextPingAt = _now + (_clock.SampleCount() < kClockBurstSamples ? _options.clockBurstIntervalMicros
                                                                             : _options.clockIntervalMicros);
        }

        // Remote objects at the interpolated time of this update
        ApplyInterpolation();

        // Out while the connection is up, a refusal's reject included
        if (_connection != nullptr && _serverPeer != N2Net::kInvalidPeer &&
            (Live() || _state == NetworkClientState::Refused) && !_disconnectPending)
        {
            const N2Net::PeerId peer = _serverPeer;
            const auto sender = [this, peer](const std::span<const std::uint8_t> packet) { _transport.Send(peer, packet); };
            _connection->Update(_now, sender);
        }

        if (_state == NetworkClientState::Refused && !_disconnectPending &&
            (_connection == nullptr || _connection->ReliableIdle() || _now - _refusedAt >= _options.refuseLingerMicros))
        {
            _disconnectPending = true;
        }
        if (_state == NetworkClientState::Failed)
        {
            _disconnectPending = true;
        }

        if (_disconnectPending)
        {
            if (_serverPeer != N2Net::kInvalidPeer)
            {
                _transport.Disconnect(_serverPeer);
            }
            _disconnectPending = false;
            _finished = true;
        }
    }

    void NetworkClient::HandleEvent(const N2Net::TransportEvent &event)
    {
        switch (event.type)
        {
        case N2Net::TransportEventType::PeerConnected:
            if (_serverPeer == N2Net::kInvalidPeer && _state == NetworkClientState::Connecting)
            {
                _serverPeer = event.peer;
                auto config = MakeWorldConnectionConfig(_options.protocolId, _transport);
                config.reliableStream = _transport.IsReliablePeer(event.peer);
                config.maxPacketSize = static_cast<std::uint32_t>(_transport.MaxPeerPacketSize(event.peer));
                _connection = std::make_unique<N2Net::Connection>(config, _now);
                _state = NetworkClientState::AwaitingHello;
                _helloWaitSince = _now;
            }
            break;
        case N2Net::TransportEventType::PeerDisconnected:
            if (_state == NetworkClientState::Connecting)
            {
                _state = NetworkClientState::Failed;
                _error = "the transport could not connect or authenticate";
                _finished = true;
                Notify(NetworkSessionEvent{.type = NetworkSessionEvent::Type::Failed, .text = _error});
                break;
            }
            if (event.peer == _serverPeer && _serverPeer != N2Net::kInvalidPeer)
            {
                if (Live())
                {
                    _state = NetworkClientState::Closed;
                    _error = "the server closed the connection";
                    Notify(NetworkSessionEvent{.type = NetworkSessionEvent::Type::Closed, .text = _error});
                }
                // The link is gone: there is nothing to disconnect and nothing more to send
                _finished = true;
            }
            break;
        case N2Net::TransportEventType::Datagram:
            if (event.peer == _serverPeer && _serverPeer != N2Net::kInvalidPeer)
            {
                OnDatagram(event.data);
            }
            break;
        }
    }

    void NetworkClient::OnDatagram(const std::span<const std::uint8_t> data)
    {
        if (_connection == nullptr || _finished)
        {
            return;
        }
        const auto handler = [this](const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
        { OnMessage(channel, message); };
        // A datagram the connection rejects is counted by it and changes nothing
        (void)_connection->OnPacket(_now, data, handler);
    }

    void NetworkClient::OnMessage(const N2Net::MessageChannel channel, const std::span<const std::uint8_t> message)
    {
        if (!Live())
        {
            return;
        }
        if (channel != N2Net::MessageChannel::ReliableOrdered)
        {
            // What the server sends on the unreliable channel: state updates and input acknowledgements
            if (!message.empty() && message[0] == kInputAckType)
            {
                OnInputAck(message);
                return;
            }
            if (!message.empty() && message[0] == kClockPongType)
            {
                OnClockPong(message);
                return;
            }
            if (!message.empty() && message[0] == kRpcServerUnreliableType)
            {
                OnRpc(message);
                return;
            }
            if (!message.empty() && message[0] == kStreamTickType)
            {
                OnStreamTick(message);
                return;
            }
            if (message.empty() || message[0] != kStateUpdateType)
            {
                Fail("the server sent a message on the unreliable channel that is not a state update, an input acknowledgement, "
                     "a clock pong or a stream tick");
                return;
            }
            if (_state != NetworkClientState::Joined)
            {
                // Until the join state is in, the client may lack the objects the update is about; the server sends
                // what is still needed again, because nothing was acknowledged
                return;
            }
            if (_replicator.OnStateUpdate(_world, message) == ClientReplicator::Result::Malformed)
            {
                Fail("the server sent a malformed state update");
            }
            return;
        }

        if (!message.empty() && message[0] == kRpcServerReliableType)
        {
            OnRpc(message);
            return;
        }

        const std::expected<ServerMessage, DecodeError> decoded = DecodeServerMessage(message);
        if (!decoded)
        {
            if (decoded.error() == DecodeError::UnsupportedVersion && _state == NetworkClientState::AwaitingHello)
            {
                const std::optional<std::uint16_t> version = PeekWireVersion(message);
                RefuseJoin(JoinRefusal::WireVersion,
                           std::format("the server speaks network wire version {} and this client speaks {}",
                                       version.value_or(0), kWireVersion));
                return;
            }
            Fail(std::format("the server sent a malformed message ({})", ToString(decoded.error())));
            return;
        }

        if (const auto *hello = std::get_if<ServerHelloMessage>(&*decoded))
        {
            OnHello(*hello);
        }
        else if (const auto *spawn = std::get_if<SpawnMessage>(&*decoded))
        {
            OnSpawn(*spawn);
        }
        else if (const auto *despawn = std::get_if<DespawnMessage>(&*decoded))
        {
            OnDespawn(*despawn);
        }
        else if (const auto *visibility = std::get_if<VisibilityMessage>(&*decoded))
        {
            if (_state != NetworkClientState::Joined) { Fail("visibility before joining"); return; }
            auto *object = _world.Find(visibility->id);
            if (!object) { Fail("visibility for an unknown object"); return; }
            object->GetGameObject().SetActive(visibility->visible && visibility->active);
        }
        else if (const auto *ready = std::get_if<WorldReadyMessage>(&*decoded))
        {
            OnWorldReady(*ready);
        }
    }

    void NetworkClient::OnRpc(const std::span<const std::uint8_t> message)
    {
        if (_state != NetworkClientState::Joined)
        {
            // Until the join state is in, the client may lack the objects the call is about
            return;
        }
        RpcHeader header;
        if (!DecodeRpcHeader(message, header) ||
            (header.type != kRpcServerReliableType && header.type != kRpcServerUnreliableType))
        {
            Fail("the server sent a malformed RPC message");
            return;
        }
        ++_rpcStats.received;
        const auto refuse = [this](std::uint64_t RpcStats::*counter, const char *why)
        {
            ++(_rpcStats.*counter);
            if (_rpcLog.Allow())
            {
                Logger::Warn(std::format("NetworkClient: RPC from the server refused: {}", why));
            }
        };
        if (_pendingRpcs.size() >= _options.rpc.maxQueued)
        {
            refuse(&RpcStats::droppedQueueFull, "the RPC queue is full");
            return;
        }
        const RpcBudget::Verdict verdict = _rpcBudget.Admit(
            _now, RpcBudgetKey(header.componentIndex, header.rpcIndex), message.size(), _options.rpc);
        if (verdict != RpcBudget::Verdict::Allowed)
        {
            refuse(&RpcStats::droppedRate, "over the RPC budget");
            return;
        }
        NetworkObject *const networked = _world.Find(header.object);
        if (networked == nullptr)
        {
            // The object may have been despawned since the server sent the call
            refuse(&RpcStats::rejectedUnknown, "no such object");
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
        // A client never runs a client-to-server RPC, whoever asks
        if (descriptor.direction == RpcDirection::ClientToServer)
        {
            refuse(&RpcStats::rejectedDirection, "the RPC is a client-to-server RPC");
            return;
        }
        if (descriptor.direction == RpcDirection::ServerToOwner && networked->GetOwner() != _localClientId)
        {
            refuse(&RpcStats::rejectedAuthority, "the RPC is for the owner of the object, which is not this client");
            return;
        }
        if (descriptor.reliable != (header.type == kRpcServerReliableType))
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
        call.sender = kServerClientId;
        call.object = header.object;
        call.componentIndex = header.componentIndex;
        call.rpcIndex = header.rpcIndex;
        call.tableHash = header.tableHash;
        if (!DecodeRpcArgs(descriptor, header.args, call.args))
        {
            refuse(&RpcStats::rejectedArguments, "the arguments do not decode or are not valid");
            return;
        }
        _pendingRpcs.push_back(std::move(call));
    }

    void NetworkClient::DispatchRpcs()
    {
        if (_pendingRpcs.empty())
        {
            return;
        }
        std::vector<PendingRpc> calls;
        calls.swap(_pendingRpcs);
        for (const PendingRpc &call : calls)
        {
            RunPendingRpc(_world, call, false, _rpcStats);
        }
    }

    RpcSendResult NetworkClient::CallRpc(Component &component, const std::string_view name,
                                         const std::vector<nlohmann::json> &args)
    {
        if (_connection == nullptr || _state != NetworkClientState::Joined || _disconnectPending)
        {
            return RpcSendResult::NoTarget;
        }
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
        if (descriptor.direction != RpcDirection::ClientToServer)
        {
            return RpcSendResult::WrongDirection;
        }
        if (networked->GetOwner() != _localClientId || _localClientId == kServerClientId)
        {
            return RpcSendResult::NotOwner;
        }
        if (_options.cosmeticRpcsOnly && !descriptor.cosmetic)
        {
            ++_rpcStats.rejectedSimulation;
            return RpcSendResult::NotSimulationSafe;
        }
        std::string error;
        const std::optional<std::vector<std::uint8_t>> message =
            EncodeRpcMessage(descriptor.reliable ? kRpcClientReliableType : kRpcClientUnreliableType,
                             networked->GetNetworkId(), rpc, HashRpcTable(*RpcTableOf(component)), args, error);
        if (!message.has_value())
        {
            if (_rpcLog.Allow())
            {
                Logger::Warn(std::format("NetworkClient::CallRpc: {}", error));
            }
            return RpcSendResult::BadArguments;
        }
        if (!_rpcBudget.AdmitSend(_now, _options.rpc))
        {
            ++_rpcStats.droppedSendBudget;
            return RpcSendResult::OverBudget;
        }
        const bool ok = descriptor.reliable
                            ? _connection->SendReliable(*message) == N2Net::SendResult::Ok
                            : message->size() <= _connection->MaxUnreliableMessageSize() &&
                                  _connection->SendUnreliable(*message) == N2Net::SendResult::Ok;
        if (!ok)
        {
            ++_rpcStats.sendFailed;
            return RpcSendResult::OverBudget;
        }
        ++_rpcStats.sent;
        return RpcSendResult::Sent;
    }

    void NetworkClient::OnInputAck(const std::span<const std::uint8_t> message)
    {
        if (_state != NetworkClientState::Joined)
        {
            return;
        }
        const std::expected<InputAckMessage, DecodeError> ack = DecodeInputAck(message);
        if (!ack)
        {
            Fail(std::format("the server sent a malformed input acknowledgement ({})", ToString(ack.error())));
            return;
        }
        if (!_input.OnAck(*ack))
        {
            Fail("the server acknowledged an input frame this client never sent");
        }
    }

    void NetworkClient::SendClockPing()
    {
        const std::uint32_t sequence = _nextPingSequence;
        // 0 is not a sequence
        _nextPingSequence = sequence == 0xFFFFFFFFu ? 1u : sequence + 1u;
        PendingPing &slot = _pendingPings[sequence % kPendingPings];
        slot.sequence = sequence;
        slot.sentAt = _now;
        if (_connection->SendUnreliable(Encode(ClockPingMessage{sequence})) == N2Net::SendResult::Ok)
        {
            ++_clockStats.pingsSent;
        }
        else
        {
            slot.sequence = 0;
        }
    }

    void NetworkClient::OnClockPong(const std::span<const std::uint8_t> message)
    {
        if (_state != NetworkClientState::Joined)
        {
            return;
        }
        const std::expected<ClockPongMessage, DecodeError> pong = DecodeClockPong(message);
        if (!pong)
        {
            Fail(std::format("the server sent a malformed clock pong ({})", ToString(pong.error())));
            return;
        }
        PendingPing &slot = _pendingPings[pong->sequence % kPendingPings];
        if (slot.sequence != pong->sequence)
        {
            ++_clockStats.pongsUnmatched;
            return;
        }
        // Answered once: a second pong for the same ping finds the slot empty
        const std::uint64_t sentAt = slot.sentAt;
        slot.sequence = 0;
        ++_clockStats.pongsReceived;
        if (_clock.AddSample(sentAt, _now, pong->tick, pong->tickMicros, pong->tickRate) != ClockSample::Accepted)
        {
            ++_clockStats.pongsRejected;
        }
    }

    void NetworkClient::OnStreamTick(const std::span<const std::uint8_t> message)
    {
        if (_state != NetworkClientState::Joined)
        {
            return;
        }
        const std::expected<StreamTickMessage, DecodeError> tick = DecodeStreamTick(message);
        if (!tick)
        {
            Fail(std::format("the server sent a malformed stream tick ({})", ToString(tick.error())));
            return;
        }
        OnStateMessage(tick->tick);
    }

    bool NetworkClient::IsLocallyOwned(const NetworkObject &object) const noexcept
    {
        return _localClientId != kServerClientId && object.GetOwner() == _localClientId;
    }

    void NetworkClient::OnStateMessage(const std::uint32_t tick)
    {
        if (_options.interpolate)
        {
            _interpolator.NoteMessage(tick);
        }
    }

    void NetworkClient::OnEntryApplied(NetworkObject &object, const std::uint32_t tick)
    {
        // The object this client controls is not shown from the server's past states (the game moves it)
        if (!_options.interpolate || IsLocallyOwned(object) || !object.HasNetworkId())
        {
            return;
        }
        const NetworkTransform *const transform = object.GetGameObject().GetComponent<NetworkTransform>();
        if (transform == nullptr)
        {
            return;
        }
        SnapshotSample sample;
        sample.tick = tick;
        sample.position = transform->GetReplicatedPosition();
        sample.rotation = transform->GetReplicatedRotation();
        sample.scale = transform->GetReplicatedScale();
        sample.teleportSerial = transform->GetTeleportSerial();
        (void)_interpolator.Add(object.GetNetworkId().value, sample);
    }

    void NetworkClient::ApplyInterpolation()
    {
        if (!_options.interpolate || _state != NetworkClientState::Joined || !_clock.Synced() ||
            _interpolator.ObjectCount() == 0)
        {
            return;
        }
        const double rate = static_cast<double>(_clock.TickRate());
        const double delayTicks = static_cast<double>(_interpolator.Options().delayMicros) * rate / kMicrosPerSecond;
        _renderTick = _clock.ServerTick(_now) - delayTicks;

        for (const std::uint32_t id : _interpolator.Ids())
        {
            NetworkObject *const object = _world.Find(NetworkId{id});
            if (object == nullptr)
            {
                _interpolator.Forget(id);
                continue;
            }
            if (IsLocallyOwned(*object) || (_interpolationFilter && !_interpolationFilter(*object)))
            {
                continue;
            }
            GameObject &gameObject = object->GetGameObject();
            const NetworkTransform *const transform = gameObject.GetComponent<NetworkTransform>();
            Positionable *const positionable = gameObject.GetPositionable();
            if (transform == nullptr || positionable == nullptr)
            {
                continue;
            }
            const std::optional<InterpolatedPose> pose = _interpolator.Evaluate(id, _renderTick, rate);
            if (!pose.has_value())
            {
                continue;
            }
            if (transform->ReplicatesPosition() && transform->ReplicatesRotation())
            {
                positionable->SetLocalPositionAndRotation(pose->position, pose->rotation);
            }
            else if (transform->ReplicatesPosition())
            {
                positionable->SetLocalPosition(pose->position);
            }
            else if (transform->ReplicatesRotation())
            {
                positionable->SetLocalRotation(pose->rotation);
            }
            if (transform->ReplicatesScale())
            {
                positionable->SetLocalScale(pose->scale);
            }
        }
    }

    std::expected<std::uint32_t, std::string> NetworkClient::QueueInput(const std::uint32_t tick,
                                                                        const std::span<const float> values)
    {
        if (_state != NetworkClientState::Joined)
        {
            return std::unexpected(std::string("input can be queued only once the client has joined"));
        }
        return _input.Queue(tick, values);
    }

    void NetworkClient::OnHello(const ServerHelloMessage &hello)
    {
        if (_state != NetworkClientState::AwaitingHello)
        {
            Fail("the server sent a second hello");
            return;
        }
        _serverFingerprint = hello.fingerprint;
        _localClientId = hello.clientId;

        const JoinRefusal mismatch = CompareFingerprints(hello.fingerprint, _localFingerprint);
        if (mismatch != JoinRefusal::None)
        {
            RefuseJoin(mismatch, DescribeRefusal(mismatch, hello.fingerprint, _localFingerprint));
            return;
        }

        ClientAcceptMessage accept;
        accept.fingerprint = _localFingerprint;
        if (_connection->SendReliable(Encode(accept)) != N2Net::SendResult::Ok)
        {
            Fail("the accept could not be queued for sending");
            return;
        }
        _state = NetworkClientState::Joining;
    }

    void NetworkClient::OnSpawn(const SpawnMessage &spawn)
    {
        if (_state != NetworkClientState::Joining && _state != NetworkClientState::Joined)
        {
            Fail("the server sent a spawn before the world was accepted");
            return;
        }
        if (spawn.id.value <= _world.SceneObjectCount() || spawn.id.value <= _lastSpawnId)
        {
            Fail(std::format("the server spawned net id {} out of order (ids of spawned objects must rise, above the {} "
                             "scene objects)",
                             spawn.id.value, _world.SceneObjectCount()));
            return;
        }
        const PrefabTable::Entry *const prefab = _options.prefabs.At(spawn.prefabIndex);
        if (prefab == nullptr)
        {
            Fail(std::format("the server spawned prefab index {}, which is not in this client's prefab table of {}",
                             spawn.prefabIndex, _options.prefabs.Size()));
            return;
        }
        const std::expected<NetworkObject *, std::string> object = _world.SpawnInstance(spawn.id, prefab->name, spawn.owner);
        if (!object)
        {
            Fail(std::format("the server's spawn of net id {} could not be applied: {}", spawn.id.value, object.error()));
            return;
        }
        _lastSpawnId = spawn.id.value;
        ++_stats.spawnsApplied;
        if (_state == NetworkClientState::Joining)
        {
            ++_joinSpawns;
        }
        NetworkSessionEvent event;
        event.type = NetworkSessionEvent::Type::Spawned;
        event.id = spawn.id;
        event.prefab = prefab->name;
        event.owner = spawn.owner;
        Notify(event);
    }

    void NetworkClient::OnDespawn(const DespawnMessage &despawn)
    {
        if (_state != NetworkClientState::Joining && _state != NetworkClientState::Joined)
        {
            Fail("the server sent a despawn before the world was accepted");
            return;
        }
        if (_state == NetworkClientState::Joining)
        {
            ++_joinDespawns;
        }
        _replicator.ForgetObject(despawn.id);
        _interpolator.Forget(despawn.id.value);
        const NetworkWorld::DespawnResult result = _world.Despawn(despawn.id);
        if (!result.found)
        {
            ++_stats.despawnsUnknown;
            return;
        }
        ++_stats.despawnsApplied;
        NetworkSessionEvent event;
        event.type = NetworkSessionEvent::Type::Despawned;
        event.id = despawn.id;
        Notify(event);
    }

    void NetworkClient::OnWorldReady(const WorldReadyMessage &ready)
    {
        if (_state != NetworkClientState::Joining)
        {
            Fail("the server sent the end of the join state at the wrong time");
            return;
        }
        if (ready.spawned != _joinSpawns || ready.destroyedSceneObjects != _joinDespawns)
        {
            Fail(std::format("the join state is not what the server said it was ({} spawn(s) and {} despawn(s) arrived, "
                             "the server sent {} and {})",
                             _joinSpawns, _joinDespawns, ready.spawned, ready.destroyedSceneObjects));
            return;
        }
        _state = NetworkClientState::Joined;
        Notify(NetworkSessionEvent{.type = NetworkSessionEvent::Type::Joined});
    }

    void NetworkClient::RefuseJoin(const JoinRefusal reason, std::string text)
    {
        if (!Live())
        {
            return;
        }
        _refusal = reason;
        _error = "Refusing to join: " + text;
        _state = NetworkClientState::Refused;
        _refusedAt = _now;
        Logger::Error(_error);

        ClientRejectMessage reject;
        reject.reason = reason;
        if (_connection != nullptr)
        {
            // Best effort: the server drops a client that never accepts anyway
            (void)_connection->SendReliable(Encode(reject));
        }
        Notify(NetworkSessionEvent{.type = NetworkSessionEvent::Type::Refused, .text = _error});
    }

    void NetworkClient::Fail(std::string text)
    {
        if (!Live())
        {
            return;
        }
        _state = NetworkClientState::Failed;
        _error = std::move(text);
        Logger::Error("NetworkClient: " + _error);
        Notify(NetworkSessionEvent{.type = NetworkSessionEvent::Type::Failed, .text = _error});
    }

    void NetworkClient::Notify(const NetworkSessionEvent &event)
    {
        if (_observer)
        {
            _observer(event);
        }
    }
}
