#include "engine/network/NetworkClient.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "engine/Logger.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkServer.hpp"
#include "engine/network/NetworkTransform.hpp"
#include "engine/physics/CharacterController.hpp"

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
          _interpolator(_options.interpolation), _dilation(_options.timeDilation)
    {
        _options.serverInputDelayTicks = std::min(_options.serverInputDelayTicks, kMaxInputDelayTicks);
        _options.jitterAllowance = std::isfinite(_options.jitterAllowance) ? std::clamp(_options.jitterAllowance, 0.0, 8.0) : 1.0;
        _localFingerprint = MakeFingerprint(_options.scenePath, _options.sceneHash, _world.SceneObjectCount(),
                                            _options.prefabs, _options.inputLayout);
        _options.clockBurstIntervalMicros = std::max<std::uint64_t>(_options.clockBurstIntervalMicros, kMinClockIntervalMicros);
        _options.clockIntervalMicros = std::max<std::uint64_t>(_options.clockIntervalMicros, kMinClockIntervalMicros);
        _options.rpc = _options.rpc.Clamped();
        _replicator.SetMessageObserver([this](const std::uint32_t tick) { OnStateMessage(tick); });
        _replicator.SetEntryObserver([this](NetworkObject &object, const std::uint32_t tick) { OnEntryApplied(object, tick); });
        if (_options.prediction.enabled)
        {
            _predictionOptions = _options.prediction.Clamped();
            auto map = CharacterInputMap::Create(_predictionOptions.mapping, _options.inputLayout);
            if (map.has_value())
            {
                _predictionMap = *map;
            }
            else
            {
                _predictionError = map.error();
                Logger::Error(std::format("Client prediction is off: {}", _predictionError));
            }
        }
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

        // The server's states for the objects this client predicts, against what it predicted
        if (_predictionMap.has_value() && _state == NetworkClientState::Joined)
        {
            ReconcilePredicted();
            ApplyPredictedPositions();
        }

        // Time dilation (opt-in): judge the lead of the game's input stamps against the suggested tick
        UpdateDilation();

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
        if (_predictionMap.has_value() && IsLocallyOwned(object))
        {
            NotePredictedEntry(object);
        }
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
        const std::expected<std::uint32_t, std::string> queued = _input.Queue(tick, values);
        if (queued.has_value())
        {
            _hasInputTick = true;
            _lastInputTick = tick;
            _lastInputAt = _now;
        }
        if (queued.has_value() && _predictionMap.has_value())
        {
            // Predict with the input the server will read: the layout's quantisation of the values
            const InputLayout &layout = _options.inputLayout;
            if (const std::optional<std::vector<std::uint8_t>> packed = layout.Pack(values); packed.has_value())
            {
                if (const std::optional<std::vector<float>> onWire = layout.Unpack(*packed); onWire.has_value())
                {
                    Predict(*onWire, tick);
                }
            }
        }
        return queued;
    }

    // ---- Input lead and time dilation (netcode stage B6 follow-up) -------------------------------------------------------

    std::optional<double> NetworkClient::SuggestedInputTickExact()
    {
        if (!_clock.Synced() || _clock.TickRate() == 0)
        {
            return std::nullopt;
        }
        const double rate = static_cast<double>(_clock.TickRate());
        const double lead = (static_cast<double>(_clock.RttMicros()) * 0.5 +
                             _options.jitterAllowance * static_cast<double>(_clock.JitterMicros())) *
                            rate / kMicrosPerSecond;
        return std::max(0.0, _clock.ServerTick(_now) + lead - static_cast<double>(_options.serverInputDelayTicks));
    }

    std::optional<std::uint32_t> NetworkClient::SuggestedInputTick()
    {
        if (!_clock.Synced() || _clock.TickRate() == 0)
        {
            return std::nullopt;
        }
        const double rate = static_cast<double>(_clock.TickRate());
        const double lead = std::ceil((static_cast<double>(_clock.RttMicros()) * 0.5 +
                                       _options.jitterAllowance * static_cast<double>(_clock.JitterMicros())) *
                                      rate / kMicrosPerSecond);
        const double tick = std::floor(_clock.ServerTick(_now)) + lead - static_cast<double>(_options.serverInputDelayTicks);
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(std::max(0.0, tick)));
    }

    std::optional<double> NetworkClient::InputLeadTicks()
    {
        if (!_hasInputTick)
        {
            return std::nullopt;
        }
        const std::optional<double> target = SuggestedInputTickExact();
        if (!target.has_value())
        {
            return std::nullopt;
        }
        // Compare modulo 2^32, as stamps are: the whole-tick part as a signed distance, the fraction as is
        const double floorTarget = std::floor(*target);
        const std::int32_t whole =
            TickDifference(_lastInputTick, static_cast<std::uint32_t>(static_cast<std::uint64_t>(floorTarget)));
        return static_cast<double>(whole) + 0.5 - (*target - floorTarget);
    }

    double NetworkClient::FixedStepScale() const noexcept
    {
        return _state == NetworkClientState::Joined ? _dilation.Scale() : 1.0;
    }

    std::int32_t NetworkClient::TakeFixedStepAdjustTicks() noexcept
    {
        return _state == NetworkClientState::Joined ? _dilation.TakeResyncTicks() : 0;
    }

    void NetworkClient::UpdateDilation()
    {
        if (!_dilation.Options().enabled)
        {
            return;
        }
        if (_state != NetworkClientState::Joined)
        {
            _dilation.Reset();
            return;
        }
        // A game that stopped stamping (paused, loading) has no lead to judge; a resync's hold is excused
        constexpr std::uint64_t kStaleMicros = 1'000'000;
        if (!_dilation.InCooldown(_now) && _hasInputTick && _now > _lastInputAt && _now - _lastInputAt > kStaleMicros)
        {
            _dilation.Reset();
            return;
        }
        if (const std::optional<double> lead = InputLeadTicks(); lead.has_value())
        {
            _dilation.Update(_now, *lead);
        }
    }

    // ---- Prediction (netcode stage B6) ---------------------------------------------------------------------------------

    namespace
    {
        Physics::CharacterController *ControllerOf(NetworkObject &object)
        {
            return object.GetGameObject().GetComponent<Physics::CharacterController>();
        }

        float Length(const Math::Vector3 &v)
        {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }
    }

    bool NetworkClient::SetPredictionTuning(const PredictionTuning &tuning) noexcept
    {
        if (!_predictionMap.has_value())
        {
            return false;
        }
        _predictionOptions = tuning.AppliedTo(_predictionOptions);
        return true;
    }

    std::size_t NetworkClient::PredictionHistorySize(const NetworkId id) const
    {
        const auto found = _predicted.find(id.value);
        return found == _predicted.end() ? 0 : found->second.history.Size();
    }

    Math::Vector3 NetworkClient::PredictionOffset(const NetworkId id) const
    {
        const auto found = _predicted.find(id.value);
        return found == _predicted.end() ? Math::Vector3(0.0f, 0.0f, 0.0f) : OffsetOf(found->second);
    }

    Math::Vector3 NetworkClient::OffsetOf(const PredictedObject &object) const
    {
        const std::uint64_t duration = _predictionOptions.smoothingMicros;
        if (duration == 0 || _now >= object.offsetSince + duration)
        {
            return Math::Vector3(0.0f, 0.0f, 0.0f);
        }
        const float remaining =
            1.0f - static_cast<float>(_now - std::min(_now, object.offsetSince)) / static_cast<float>(duration);
        return Math::Vector3(object.offsetStart.x * remaining, object.offsetStart.y * remaining,
                             object.offsetStart.z * remaining);
    }

    void NetworkClient::ScanPredicted()
    {
        _predictRescan = false;
        for (auto it = _predicted.begin(); it != _predicted.end();)
        {
            NetworkObject *const object = _world.Find(NetworkId{it->first});
            if (object == nullptr || !IsLocallyOwned(*object) || ControllerOf(*object) == nullptr)
            {
                it = _predicted.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (const auto &[id, object] : _world.Objects())
        {
            if (_predicted.size() >= kMaxPredictedObjects)
            {
                break;
            }
            if (_predicted.contains(id) || !IsLocallyOwned(*object))
            {
                continue;
            }
            if (Physics::CharacterController *const controller = ControllerOf(*object))
            {
                controller->SetExternallyDriven(true);
                controller->SetGroundMap(&_world);
                _predicted.emplace(id, PredictedObject(_predictionOptions.historyTicks));
            }
        }
    }

    void NetworkClient::Predict(const std::vector<float> &values, const std::uint32_t tick)
    {
        if (_predictRescan)
        {
            ScanPredicted();
        }
        const Physics::CharacterInput input = _predictionMap->Map(values);
        for (auto &[id, predicted] : _predicted)
        {
            NetworkObject *const object = _world.Find(NetworkId{id});
            Physics::CharacterController *const controller = object != nullptr ? ControllerOf(*object) : nullptr;
            if (controller == nullptr)
            {
                _predictRescan = true;
                continue;
            }
            if (!predicted.synced)
            {
                continue;
            }
            (void)controller->SetInput(input);
            if (!controller->Step(_predictionOptions.stepSeconds))
            {
                continue;
            }
            ++_predictionStats.inputsPredicted;
            (void)predicted.history.Record(tick, input, _predictionOptions.stepSeconds, controller->GetState());
        }
        ApplyPredictedPositions();
    }

    void NetworkClient::NotePredictedEntry(NetworkObject &object)
    {
        if (!object.HasNetworkId())
        {
            return;
        }
        const auto found = _predicted.find(object.GetNetworkId().value);
        Physics::CharacterController *const controller = ControllerOf(object);
        if (found == _predicted.end() || controller == nullptr)
        {
            return;
        }
        PredictedObject &predicted = found->second;
        if (!predicted.synced)
        {
            // The first state of the object: from here on the controller starts from where the server's is, not from where
            // the instance happened to be placed (the frames the server applied before this are not predicted)
            const Physics::CharacterState first = controller->GetReplicatedState();
            Physics::CharacterState start;
            start.position = first.position;
            start.velocity = Math::Vector3(0.0f, first.velocity.y, 0.0f);
            start.grounded = first.grounded;
            start.ground = first.ground;
            start.groundLocal = first.groundLocal;
            if (controller->SetState(controller->Anchored(start)))
            {
                predicted.synced = true;
            }
        }
        const std::optional<std::uint32_t> inputTick = _replicator.CurrentInputTick();
        if (!inputTick.has_value())
        {
            return;
        }
        if (predicted.hasPending && TickBefore(*inputTick, predicted.pendingTick))
        {
            return;
        }
        predicted.hasPending = true;
        predicted.pendingTick = *inputTick;
        // The server's state in this machine's frame: a ride on a body this client has is placed on its copy of the body
        // (the offset is the server's, the world position follows this client's pose of the body); a ride on a body it
        // does not have (not replicated to it, not yet spawned, gone) is the world frame, from the server's position
        predicted.pendingState = controller->Anchored(controller->GetReplicatedState());
    }

    void NetworkClient::ReconcilePredicted()
    {
        if (_predictRescan)
        {
            ScanPredicted();
        }
        for (auto &[id, predicted] : _predicted)
        {
            if (!predicted.hasPending)
            {
                continue;
            }
            predicted.hasPending = false;
            NetworkObject *const object = _world.Find(NetworkId{id});
            Physics::CharacterController *const controller = object != nullptr ? ControllerOf(*object) : nullptr;
            if (controller == nullptr)
            {
                continue;
            }
            // What the object shows now: the state and the correction still being shown
            // (A rider is placed on its platform as the platform is now: the state's world position was set at the last
            // step, and the platform has moved since, which is not part of the correction)
            const Math::Vector3 shown = controller->Anchored(controller->GetState()).position + OffsetOf(predicted);
            const PredictionStepper stepper = [controller](const Physics::CharacterState &state,
                                                           const Physics::CharacterInput &input, const float dt)
            { return controller->Simulate(state, input, dt); };

            const ReconcileResult result =
                predicted.history.Reconcile(predicted.pendingTick, predicted.pendingState, stepper, _predictionOptions);
            if (result.outcome == ReconcileOutcome::Ignored)
            {
                ++_predictionStats.statesIgnored;
                continue;
            }
            if (result.compared)
            {
                ++_predictionStats.comparisons;
                _predictionStats.maxError = std::max(_predictionStats.maxError, result.error);
            }
            if (result.outcome == ReconcileOutcome::Match)
            {
                ++_predictionStats.matches;
                continue;
            }
            if (!controller->SetState(result.state))
            {
                continue;
            }
            const Math::Vector3 delta = shown - controller->GetState().position;
            const bool snap = result.outcome == ReconcileOutcome::Overflow ||
                              result.error > _predictionOptions.snapDistance ||
                              Length(delta) > _predictionOptions.snapDistance;
            if (result.outcome == ReconcileOutcome::Overflow)
            {
                ++_predictionStats.windowOverflows;
            }
            else
            {
                ++_predictionStats.corrections;
                _predictionStats.replayedSteps += result.replayed;
            }
            if (snap)
            {
                ++_predictionStats.snaps;
                predicted.offsetStart = Math::Vector3(0.0f, 0.0f, 0.0f);
            }
            else
            {
                predicted.offsetStart = delta;
            }
            predicted.offsetSince = _now;
        }
    }

    void NetworkClient::ApplyPredictedPositions()
    {
        if (!_predictionOptions.applySmoothing)
        {
            return;
        }
        for (const auto &[id, predicted] : _predicted)
        {
            NetworkObject *const object = _world.Find(NetworkId{id});
            Physics::CharacterController *const controller = object != nullptr ? ControllerOf(*object) : nullptr;
            Positionable *const positionable = object != nullptr ? object->GetGameObject().GetPositionable() : nullptr;
            if (controller == nullptr || positionable == nullptr)
            {
                continue;
            }
            positionable->SetPosition(controller->GetState().position + OffsetOf(predicted));
        }
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
        _predictRescan = true;
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
        _predictRescan = true;
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
