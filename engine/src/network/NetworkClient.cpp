#include "engine/network/NetworkClient.hpp"

#include <format>

#include "engine/Logger.hpp"
#include "engine/GameObject.hpp"
#include "engine/network/NetworkServer.hpp"

namespace N2Engine::Network
{
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
        : _transport(transport), _world(world), _options(std::move(options))
    {
        _localFingerprint =
            MakeFingerprint(_options.scenePath, _options.sceneHash, _world.SceneObjectCount(), _options.prefabs);
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
            // The one thing the server sends on the unreliable channel is a state update
            if (message.empty() || message[0] != kStateUpdateType)
            {
                Fail("the server sent a message on the unreliable channel that is not a state update");
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
