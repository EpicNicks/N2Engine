#include "engine/network/RollbackWorld.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

#include <math/Random.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/BodyState.hpp"
#include "engine/physics/IPhysicsBackend.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

namespace N2Engine::Network
{
    namespace
    {
        /// True when the root or anything under it has a Rigidbody (a pooled object is then handed out inactive and its
        /// bodies are reset by CommitSpawns)
        bool HasAnyRigidbody(GameObject &root)
        {
            if (root.GetComponent<Physics::Rigidbody>() != nullptr)
            {
                return true;
            }
            for (const GameObject::Ptr &child : root.GetChildrenRecursive())
            {
                if (child && child->GetComponent<Physics::Rigidbody>() != nullptr)
                {
                    return true;
                }
            }
            return false;
        }

        std::vector<std::optional<Transform>> DescendantTransforms(const GameObject &root)
        {
            std::vector<std::optional<Transform>> out;
            for (const GameObject::Ptr &child : root.GetChildrenRecursive())
            {
                const Positionable *const positionable = child ? child->GetPositionable() : nullptr;
                out.push_back(positionable != nullptr ? std::optional<Transform>(positionable->GetLocalTransform())
                                                      : std::nullopt);
            }
            return out;
        }

        // ===== Snapshot bytes =====

        constexpr std::uint32_t kSnapshotMagic = 0x5357324Eu; // "N2WS" little-endian
        constexpr std::uint32_t kSnapshotFormat = 1;

        std::uint64_t Fnv1a(const std::span<const std::uint8_t> bytes)
        {
            std::uint64_t hash = 14695981039346656037ull;
            for (const std::uint8_t byte : bytes)
            {
                hash = (hash ^ byte) * 1099511628211ull;
            }
            return hash;
        }

        class Writer
        {
        public:
            explicit Writer(std::vector<std::uint8_t> &out) : _out(out) {}
            void U8(const std::uint8_t value) { _out.push_back(value); }
            void U16(const std::uint16_t value)
            {
                U8(static_cast<std::uint8_t>(value & 0xFFu));
                U8(static_cast<std::uint8_t>(value >> 8));
            }
            void U32(const std::uint32_t value)
            {
                for (int shift = 0; shift < 32; shift += 8)
                {
                    U8(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
                }
            }
            void U64(const std::uint64_t value)
            {
                U32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
                U32(static_cast<std::uint32_t>(value >> 32));
            }
            void F32(const float value)
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &value, sizeof(bits));
                U32(bits);
            }
            void Bytes(const std::span<const std::uint8_t> bytes) { _out.insert(_out.end(), bytes.begin(), bytes.end()); }

        private:
            std::vector<std::uint8_t> &_out;
        };

        class Reader
        {
        public:
            explicit Reader(const std::span<const std::uint8_t> bytes) : _bytes(bytes) {}
            [[nodiscard]] bool Ok() const { return _ok; }
            [[nodiscard]] std::size_t Remaining() const { return _ok ? _bytes.size() - _at : 0; }
            std::uint8_t U8()
            {
                if (!_ok || _at >= _bytes.size())
                {
                    _ok = false;
                    return 0;
                }
                return _bytes[_at++];
            }
            std::uint16_t U16()
            {
                const std::uint16_t low = U8();
                return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(U8()) << 8));
            }
            std::uint32_t U32()
            {
                std::uint32_t value = 0;
                for (int shift = 0; shift < 32; shift += 8)
                {
                    value |= static_cast<std::uint32_t>(U8()) << shift;
                }
                return value;
            }
            std::uint64_t U64()
            {
                const std::uint64_t low = U32();
                return low | (static_cast<std::uint64_t>(U32()) << 32);
            }
            float F32()
            {
                const std::uint32_t bits = U32();
                float value = 0.0f;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            }
            std::span<const std::uint8_t> Bytes(const std::size_t count)
            {
                if (!_ok || count > Remaining())
                {
                    _ok = false;
                    return {};
                }
                const std::span<const std::uint8_t> out = _bytes.subspan(_at, count);
                _at += count;
                return out;
            }

        private:
            std::span<const std::uint8_t> _bytes;
            std::size_t _at = 0;
            bool _ok = true;
        };

        void WriteVector(Writer &writer, const Math::Vector3 &value)
        {
            writer.F32(value.x);
            writer.F32(value.y);
            writer.F32(value.z);
        }

        Math::Vector3 ReadVector(Reader &reader)
        {
            const float x = reader.F32();
            const float y = reader.F32();
            const float z = reader.F32();
            return Math::Vector3(x, y, z);
        }

        void RegisterObjects(const NetworkWorld &world, ReferenceResolver &resolver)
        {
            for (const auto &entry : world.Objects())
            {
                GameObject &gameObject = entry.second->GetGameObject();
                resolver.RegisterGameObject(gameObject.GetUUID(), &gameObject);
            }
        }
    }

    RollbackWorld::RollbackWorld(NetworkWorld &world, Physics::IPhysicsBackend *physics) noexcept
        : _world(world), _physics(physics)
    {
    }

    bool RollbackWorld::PhysicsActive() const
    {
        return _physics != nullptr && _physics->SupportsSnapshot();
    }

    bool RollbackWorld::Save(const std::uint32_t tick, WorldSnapshot &out)
    {
        out.tick = tick;
        out.nextId = _world.NextId();
        out.objects.clear();
        out.objects.reserve(_world.Count());
        for (const auto &entry : _world.Objects())
        {
            NetworkObject &object = *entry.second;
            GameObject &gameObject = object.GetGameObject();
            WorldSnapshot::Object saved;
            saved.id = entry.first;
            saved.owner = object.GetOwner();
            saved.spawned = object.IsSpawned();
            saved.prefab = object.GetPrefabName();
            saved.active = gameObject.IsActive();
            saved.values = CaptureObjectValues(_layouts.Get(object.GetNetworkId(), gameObject), gameObject);
            if (_physics != nullptr)
            {
                if (const Physics::Rigidbody *const body = gameObject.GetComponent<Physics::Rigidbody>();
                    body != nullptr && body->GetHandle().IsValid())
                {
                    Physics::BodyState state;
                    if (_physics->GetBodyState(body->GetHandle(), state))
                    {
                        saved.body = state;
                    }
                }
            }
            out.objects.push_back(std::move(saved));
        }
        out.rng = Math::Random::SeededState();
        out.streams.clear();
        for (const Math::DeterministicRandom *const stream : _streams)
        {
            out.streams.push_back(stream->State());
        }
        out.hasPhysics = false;
        out.physics.clear();
        if (PhysicsActive())
        {
            if (!_physics->SaveState(out.physics))
            {
                _lastError = "the physics backend refused SaveState (see the log)";
                return false;
            }
            out.hasPhysics = true;
        }
        ++_stats.saves;
        return true;
    }

    std::expected<void, std::string> RollbackWorld::Restore(const WorldSnapshot &snapshot)
    {
        if (snapshot.hasPhysics && !PhysicsActive())
        {
            return std::unexpected(std::string("the snapshot has a physics part but this world has no snapshot-capable backend"));
        }

        if (snapshot.streams.size() != _streams.size())
        {
            return std::unexpected(std::string("the snapshot holds a different number of random streams than are registered"));
        }

        std::set<std::uint32_t> wanted;
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            wanted.insert(saved.id);
        }

        // Check everything that can be refused before anything changes
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            NetworkObject *object = _world.Find(NetworkId{saved.id});
            if (object == nullptr)
            {
                const NetworkWorld::ParkedObject *const parked = _world.FindParked(NetworkId{saved.id});
                if (parked == nullptr)
                {
                    return std::unexpected(std::format("net object {} of the snapshot is neither registered nor parked "
                                                       "(released before the snapshot was dropped)",
                                                       saved.id));
                }
                object = parked->object;
            }
            GameObject &gameObject = object->GetGameObject();
            if (_layouts.Get(NetworkId{saved.id}, gameObject).hash != saved.values.layoutHash)
            {
                return std::unexpected(std::format("the replicated layout of net object {} changed since the snapshot", saved.id));
            }
        }

        _uncommitted.clear();

        // Objects that did not exist at the restore point: park them (undo the spawn)
        std::vector<std::uint32_t> extras;
        for (const auto &entry : _world.Objects())
        {
            if (!wanted.contains(entry.first))
            {
                extras.push_back(entry.first);
            }
        }
        for (const std::uint32_t id : extras)
        {
            NetworkObject *const object = _world.Find(NetworkId{id});
            const bool pooled = object->IsSpawned();
            (void)_world.Park(NetworkId{id}, NetworkWorld::ParkReason::SpawnUndone, snapshot.tick);
            if (!pooled)
            {
                // A scene object that did not exist at the snapshot cannot be spawned again by a resimulation: it goes
                // (its body did not exist at the snapshot either, so it may be destroyed)
                _world.ReleaseParked(NetworkId{id});
                ++_stats.released;
            }
            ++_stats.spawnsUndone;
        }

        // Objects that existed and were despawned since: bring them back
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            if (_world.Find(NetworkId{saved.id}) == nullptr)
            {
                (void)_world.Unpark(NetworkId{saved.id});
                ++_stats.despawnsUndone;
            }
        }

        // The active flags first (they enable and disable bodies), then the fields
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            _world.Find(NetworkId{saved.id})->GetGameObject().SetActive(saved.active);
        }
        ReferenceResolver resolver;
        RegisterObjects(_world, resolver);
        std::string refusedNote;
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            NetworkObject &object = *_world.Find(NetworkId{saved.id});
            GameObject &gameObject = object.GetGameObject();
            std::size_t refused = 0;
            if (!ApplyObjectValues(_layouts.Get(NetworkId{saved.id}, gameObject), gameObject, saved.values, resolver, refused))
            {
                refusedNote += std::format(" net object {} refused {} value(s);", saved.id, refused);
            }
        }
        _world.SetNextId(snapshot.nextId);
        Math::Random::RestoreSeededState(snapshot.rng);
        for (std::size_t i = 0; i < _streams.size(); ++i)
        {
            _streams[i]->Seed(snapshot.streams[i]);
        }

        if (snapshot.hasPhysics)
        {
            if (!_physics->RestoreState(std::span<const std::byte>(snapshot.physics.data(), snapshot.physics.size())))
            {
                return std::unexpected(std::string("the physics backend refused RestoreState (see the log)"));
            }
            _physics->SyncTransforms();
        }
        ForgetStalePristine();
        ++_stats.restores;
        if (!refusedNote.empty())
        {
            return std::unexpected("a component refused a restored value:" + refusedNote);
        }
        return {};
    }

    std::expected<NetworkObject *, std::string> RollbackWorld::Spawn(const std::string_view prefab, const ClientId owner,
                                                                    const std::uint32_t tick)
    {
        (void)tick;
        const NetworkId id = _world.AllocateId();
        if (!id.IsValid())
        {
            return std::unexpected(std::string("the net id counter is spent"));
        }

        if (const NetworkWorld::ParkedObject *const parked = _world.FindParked(id); parked != nullptr)
        {
            NetworkObject &object = *parked->object;
            const auto pristine = _pristine.find(id.value);
            const std::vector<GameObject::Ptr> descendants =
                object.GetGameObject().GetChildrenRecursive();
            const bool reusable = object.IsSpawned() && object.GetPrefabName() == prefab && object.GetOwner() == owner &&
                                  pristine != _pristine.end() &&
                                  descendants.size() == pristine->second.descendants.size();
            if (reusable)
            {
                // Back to the state right after instantiation while it is still parked: the transforms of its whole
                // hierarchy, its replicated fields, then (as it becomes active again, which enables each Rigidbody's body
                // at its GameObject's pose) its bodies without the velocity and sleep state of their earlier life
                GameObject &gameObject = object.GetGameObject();
                if (pristine->second.transform.has_value())
                {
                    if (Positionable *const positionable = gameObject.GetPositionable(); positionable != nullptr)
                    {
                        positionable->SetLocalTransform(*pristine->second.transform);
                    }
                }
                for (std::size_t i = 0; i < descendants.size(); ++i)
                {
                    Positionable *const positionable = descendants[i] ? descendants[i]->GetPositionable() : nullptr;
                    if (positionable != nullptr && pristine->second.descendants[i].has_value())
                    {
                        positionable->SetLocalTransform(*pristine->second.descendants[i]);
                    }
                }
                ReferenceResolver resolver;
                RegisterObjects(_world, resolver);
                resolver.RegisterGameObject(gameObject.GetUUID(), &gameObject);
                std::size_t refused = 0;
                if (ApplyObjectValues(_layouts.Get(id, gameObject), gameObject, pristine->second.values, resolver, refused))
                {
                    // An object with a Rigidbody stays inactive (its body disabled) until CommitSpawns: the game sets its
                    // pose after Spawn returns, the way a fresh instance gets its body only when the attach queue runs
                    const bool deferred = HasAnyRigidbody(gameObject);
                    NetworkObject *const back = _world.Unpark(id, false);
                    if (deferred)
                    {
                        _uncommitted.push_back(id.value);
                    }
                    else
                    {
                        gameObject.SetActive(pristine->second.active);
                    }
                    ++_stats.spawnsPooled;
                    return back;
                }
                // Could not reset it: do not hand out a dirty object
            }
            _world.ReleaseParked(id);
            ++_stats.released;
            _pristine.erase(id.value);
        }

        std::expected<NetworkObject *, std::string> spawned = _world.SpawnInstance(id, prefab, owner);
        if (!spawned.has_value())
        {
            return spawned;
        }
        NetworkObject &object = **spawned;
        GameObject &gameObject = object.GetGameObject();
        Pristine pristine;
        pristine.active = gameObject.IsActive();
        pristine.values = CaptureObjectValues(_layouts.Get(id, gameObject), gameObject);
        if (const Positionable *const positionable = gameObject.GetPositionable(); positionable != nullptr)
        {
            pristine.transform = positionable->GetLocalTransform();
        }
        pristine.descendants = DescendantTransforms(gameObject);
        _pristine[id.value] = std::move(pristine);
        return spawned;
    }

    void RollbackWorld::CommitSpawns()
    {
        for (const std::uint32_t id : _uncommitted)
        {
            NetworkObject *const object = _world.Find(NetworkId{id});
            const auto pristine = _pristine.find(id);
            if (object == nullptr || pristine == _pristine.end())
            {
                continue; // despawned again, or released
            }
            GameObject &gameObject = object->GetGameObject();
            gameObject.SetActive(pristine->second.active);
            ResetBody(gameObject);
        }
        _uncommitted.clear();
    }

    void RollbackWorld::ResetBody(GameObject &gameObject) const
    {
        if (_physics == nullptr)
        {
            return;
        }
        const auto reset = [this](GameObject &target)
        {
            Physics::Rigidbody *const body = target.GetComponent<Physics::Rigidbody>();
            const Positionable *const positionable = target.GetPositionable();
            if (body == nullptr || positionable == nullptr || !body->GetHandle().IsValid())
            {
                return;
            }
            Physics::BodyState state;
            state.position = positionable->GetPosition();
            state.rotation = positionable->GetRotation();
            // A static body takes no state (the call is refused and there is nothing to reset)
            (void)_physics->SetBodyState(body->GetHandle(), state);
        };
        reset(gameObject);
        for (const GameObject::Ptr &child : gameObject.GetChildrenRecursive())
        {
            if (child)
            {
                reset(*child);
            }
        }
    }

    bool RollbackWorld::Despawn(const NetworkId id, const std::uint32_t tick)
    {
        return _world.Park(id, NetworkWorld::ParkReason::Despawned, tick);
    }

    std::size_t RollbackWorld::ReleaseConfirmed(const std::uint32_t oldestSnapshotTick)
    {
        std::vector<std::uint32_t> release;
        for (const auto &entry : _world.Parked())
        {
            if (entry.second.reason == NetworkWorld::ParkReason::Despawned &&
                static_cast<std::int32_t>(entry.second.tick - oldestSnapshotTick) < 0)
            {
                release.push_back(entry.first);
            }
        }
        for (const std::uint32_t id : release)
        {
            _world.ReleaseParked(NetworkId{id});
            ++_stats.released;
        }
        ForgetStalePristine();
        return release.size();
    }

    std::size_t RollbackWorld::ReleasePool()
    {
        std::vector<std::uint32_t> release;
        for (const auto &entry : _world.Parked())
        {
            if (entry.second.reason == NetworkWorld::ParkReason::SpawnUndone)
            {
                release.push_back(entry.first);
            }
        }
        for (const std::uint32_t id : release)
        {
            _world.ReleaseParked(NetworkId{id});
            ++_stats.released;
        }
        ForgetStalePristine();
        return release.size();
    }

    std::expected<JoinRestoreResult, std::string> RollbackWorld::RestoreForJoin(const WorldSnapshot &snapshot)
    {
        JoinRestoreResult result;
        Scene *const scene = _world.GetScene();
        if (scene == nullptr)
        {
            return std::unexpected(std::string("the network world is not bound to a scene"));
        }
        if (snapshot.streams.size() != _streams.size())
        {
            return std::unexpected(std::string("the snapshot holds a different number of random streams than are registered"));
        }
        std::set<std::uint32_t> wanted;
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            wanted.insert(saved.id);
            if (!saved.spawned && _world.Find(NetworkId{saved.id}) == nullptr &&
                _world.FindParked(NetworkId{saved.id}) == nullptr)
            {
                return std::unexpected(std::format("scene object {} of the snapshot is not in this scene (another scene, "
                                                   "or another version of it)",
                                                   saved.id));
            }
        }

        // What this world has and the snapshot does not: gone on the other peers (a despawn, a destroyed scene object)
        std::vector<std::uint32_t> extras;
        for (const auto &entry : _world.Objects())
        {
            if (!wanted.contains(entry.first))
            {
                extras.push_back(entry.first);
            }
        }
        for (const std::uint32_t id : extras)
        {
            (void)_world.Despawn(NetworkId{id});
            ++result.removed;
        }
        std::vector<std::uint32_t> parked;
        for (const auto &entry : _world.Parked())
        {
            if (!wanted.contains(entry.first))
            {
                parked.push_back(entry.first);
            }
        }
        for (const std::uint32_t id : parked)
        {
            _world.ReleaseParked(NetworkId{id});
            ++_stats.released;
        }

        // What the snapshot has and this world does not: spawned on the other peers before the snapshot
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            if (_world.Find(NetworkId{saved.id}) != nullptr || _world.FindParked(NetworkId{saved.id}) != nullptr)
            {
                continue;
            }
            const auto spawned = _world.SpawnInstance(NetworkId{saved.id}, saved.prefab, saved.owner);
            if (!spawned.has_value())
            {
                return std::unexpected(std::format("net object {} (prefab {}) could not be instantiated: {}", saved.id,
                                                   saved.prefab, spawned.error()));
            }
            ++result.spawned;
        }
        // Their bodies come into existence here, the despawned ones go
        scene->ProcessAttachQueue();
        scene->ProcessDestroyed();

        WorldSnapshot objectsOnly = snapshot;
        objectsOnly.hasPhysics = false;
        objectsOnly.physics.clear();
        if (const auto restored = Restore(objectsOnly); !restored.has_value())
        {
            return std::unexpected(restored.error());
        }

        if (snapshot.hasPhysics && PhysicsActive() &&
            _physics->RestoreState(std::span<const std::byte>(snapshot.physics.data(), snapshot.physics.size())))
        {
            result.exactPhysics = true;
        }
        else if (_physics != nullptr)
        {
            for (const WorldSnapshot::Object &saved : snapshot.objects)
            {
                NetworkObject *const object = _world.Find(NetworkId{saved.id});
                if (object == nullptr || !saved.body.has_value())
                {
                    continue;
                }
                if (const Physics::Rigidbody *const body = object->GetGameObject().GetComponent<Physics::Rigidbody>();
                    body != nullptr && body->GetHandle().IsValid())
                {
                    (void)_physics->SetBodyState(body->GetHandle(), *saved.body);
                }
            }
        }
        if (_physics != nullptr)
        {
            _physics->SyncTransforms();
        }
        return result;
    }

    std::vector<std::uint8_t> SerializeWorldSnapshot(const WorldSnapshot &snapshot)
    {
        std::vector<std::uint8_t> out;
        Writer writer(out);
        writer.U32(kSnapshotMagic);
        writer.U32(kSnapshotFormat);
        writer.U32(snapshot.tick);
        writer.U32(snapshot.nextId);
        writer.U8(snapshot.rng.has_value() ? 1 : 0);
        writer.U32(snapshot.rng.value_or(0));
        writer.U32(static_cast<std::uint32_t>(snapshot.streams.size()));
        for (const std::uint32_t stream : snapshot.streams)
        {
            writer.U32(stream);
        }
        writer.U32(static_cast<std::uint32_t>(snapshot.objects.size()));
        for (const WorldSnapshot::Object &object : snapshot.objects)
        {
            writer.U32(object.id);
            writer.U32(object.owner);
            writer.U8(object.spawned ? 1 : 0);
            writer.U8(object.active ? 1 : 0);
            const std::size_t prefabBytes = std::min<std::size_t>(object.prefab.size(), 0xFFFFu);
            writer.U16(static_cast<std::uint16_t>(prefabBytes));
            writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(object.prefab.data()), prefabBytes));
            writer.U16(object.values.layoutHash);
            // A field is [value] when the component wrote it and null when it did not, so a written null stays a value
            nlohmann::json fields = nlohmann::json::array();
            for (const std::optional<nlohmann::json> &field : object.values.fields)
            {
                fields.push_back(field.has_value() ? nlohmann::json::array({*field}) : nlohmann::json());
            }
            const std::vector<std::uint8_t> cbor = nlohmann::json::to_cbor(fields);
            writer.U32(static_cast<std::uint32_t>(cbor.size()));
            writer.Bytes(cbor);
            writer.U8(object.body.has_value() ? 1 : 0);
            if (object.body.has_value())
            {
                const Physics::BodyState &body = *object.body;
                WriteVector(writer, body.position);
                writer.F32(body.rotation.w);
                writer.F32(body.rotation.x);
                writer.F32(body.rotation.y);
                writer.F32(body.rotation.z);
                WriteVector(writer, body.linearVelocity);
                WriteVector(writer, body.angularVelocity);
                writer.U8(body.sleeping ? 1 : 0);
            }
        }
        writer.U8(snapshot.hasPhysics ? 1 : 0);
        writer.U32(static_cast<std::uint32_t>(snapshot.physics.size()));
        writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(snapshot.physics.data()),
                                                   snapshot.physics.size()));
        writer.U64(Fnv1a(out));
        return out;
    }

    std::expected<WorldSnapshot, std::string> DeserializeWorldSnapshot(const std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() > kMaxSerializedSnapshotBytes)
        {
            return std::unexpected(std::string("the snapshot is larger than kMaxSerializedSnapshotBytes"));
        }
        if (bytes.size() < 16)
        {
            return std::unexpected(std::string("the snapshot is too short"));
        }
        const std::span<const std::uint8_t> body = bytes.first(bytes.size() - 8);
        Reader tail(bytes.last(8));
        if (tail.U64() != Fnv1a(body))
        {
            return std::unexpected(std::string("the snapshot's checksum does not match"));
        }
        Reader reader(body);
        if (reader.U32() != kSnapshotMagic)
        {
            return std::unexpected(std::string("not a world snapshot (magic)"));
        }
        if (const std::uint32_t format = reader.U32(); format != kSnapshotFormat)
        {
            return std::unexpected(std::format("world snapshot format {} (this build reads {})", format, kSnapshotFormat));
        }
        WorldSnapshot snapshot;
        snapshot.tick = reader.U32();
        snapshot.nextId = reader.U32();
        const bool hasRng = reader.U8() != 0;
        const std::uint32_t rng = reader.U32();
        if (hasRng)
        {
            snapshot.rng = rng;
        }
        const std::uint32_t streams = reader.U32();
        if (!reader.Ok() || streams > reader.Remaining() / 4)
        {
            return std::unexpected(std::string("the snapshot's random stream count is impossible"));
        }
        for (std::uint32_t i = 0; i < streams; ++i)
        {
            snapshot.streams.push_back(reader.U32());
        }
        const std::uint32_t objects = reader.U32();
        if (!reader.Ok() || objects > kMaxNetworkObjects)
        {
            return std::unexpected(std::string("the snapshot holds more objects than kMaxNetworkObjects"));
        }
        std::uint32_t previous = 0;
        for (std::uint32_t i = 0; i < objects; ++i)
        {
            WorldSnapshot::Object object;
            object.id = reader.U32();
            object.owner = reader.U32();
            object.spawned = reader.U8() != 0;
            object.active = reader.U8() != 0;
            const std::uint16_t prefabBytes = reader.U16();
            const std::span<const std::uint8_t> prefab = reader.Bytes(prefabBytes);
            object.prefab.assign(reinterpret_cast<const char *>(prefab.data()), prefab.size());
            object.values.layoutHash = reader.U16();
            const std::uint32_t cborBytes = reader.U32();
            const std::span<const std::uint8_t> cbor = reader.Bytes(cborBytes);
            if (!reader.Ok())
            {
                return std::unexpected(std::format("the snapshot is cut short in object {}", i));
            }
            if (object.id == 0 || (i > 0 && object.id <= previous))
            {
                return std::unexpected(std::string("the snapshot's objects are not in ascending id order"));
            }
            previous = object.id;
            const nlohmann::json fields = nlohmann::json::from_cbor(cbor.begin(), cbor.end(), true, false);
            if (fields.is_discarded() || !fields.is_array())
            {
                return std::unexpected(std::format("net object {}: its fields are not a CBOR list", object.id));
            }
            for (const nlohmann::json &field : fields)
            {
                if (field.is_null())
                {
                    object.values.fields.emplace_back(std::nullopt);
                }
                else if (field.is_array() && field.size() == 1)
                {
                    object.values.fields.emplace_back(field[0]);
                }
                else
                {
                    return std::unexpected(std::format("net object {}: a field is neither [value] nor null", object.id));
                }
            }
            if (reader.U8() != 0)
            {
                Physics::BodyState state;
                state.position = ReadVector(reader);
                const float w = reader.F32();
                const float x = reader.F32();
                const float y = reader.F32();
                const float z = reader.F32();
                state.rotation = Math::Quaternion(w, x, y, z);
                state.linearVelocity = ReadVector(reader);
                state.angularVelocity = ReadVector(reader);
                state.sleeping = reader.U8() != 0;
                object.body = state;
            }
            snapshot.objects.push_back(std::move(object));
        }
        snapshot.hasPhysics = reader.U8() != 0;
        const std::uint32_t physicsBytes = reader.U32();
        const std::span<const std::uint8_t> physics = reader.Bytes(physicsBytes);
        if (!reader.Ok() || reader.Remaining() != 0)
        {
            return std::unexpected(std::string("the snapshot is cut short or has bytes after its physics part"));
        }
        snapshot.physics.resize(physics.size());
        if (!physics.empty())
        {
            std::memcpy(snapshot.physics.data(), physics.data(), physics.size());
        }
        return snapshot;
    }

    void RollbackWorld::ForgetStalePristine()
    {
        for (auto it = _pristine.begin(); it != _pristine.end();)
        {
            if (_world.Find(NetworkId{it->first}) == nullptr && _world.FindParked(NetworkId{it->first}) == nullptr)
            {
                it = _pristine.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
}
