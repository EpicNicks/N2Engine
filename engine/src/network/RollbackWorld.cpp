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
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/sceneManagement/Scene.hpp"
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

        /// The scene's 2D physics world, or null (no scene, no 2D component, or a build without Box2D)
        Physics2D::Physics2DWorld *Live2DWorld(const NetworkWorld &world)
        {
            const Scene *const scene = world.GetScene();
            return scene != nullptr ? scene->GetPhysics2DWorld() : nullptr;
        }

        /// True when a 2D component of the object lost its body (a restore of the 2D world drops the bodies made after the
        /// restored instant, which is every body of an object spawned after the snapshot): such an object cannot be pooled,
        /// a fresh instance is made instead and gets a new body, as it did the first time
        bool HasLost2DBody(GameObject &root)
        {
            const auto lost = [](GameObject &object)
            {
                for (const Physics2D::Rigidbody2D *const body : object.GetComponents<Physics2D::Rigidbody2D>())
                {
                    if (!body->GetHandle().IsValid())
                    {
                        return true;
                    }
                }
                for (const Physics2D::Collider2D *const collider : object.GetComponents<Physics2D::Collider2D>())
                {
                    if (!collider->GetHandle().IsValid())
                    {
                        return true;
                    }
                }
                return false;
            };
            if (lost(root))
            {
                return true;
            }
            for (const GameObject::Ptr &child : root.GetChildrenRecursive())
            {
                if (child && lost(*child))
                {
                    return true;
                }
            }
            return false;
        }

        /// The root (node 0) and its descendants in GetChildrenRecursive order (node k is the (k-1)th)
        std::vector<GameObject *> NodesOf(GameObject &root)
        {
            std::vector<GameObject *> nodes{&root};
            if (root.GetChildCount() > 0)
            {
                for (const GameObject::Ptr &child : root.GetChildrenRecursive())
                {
                    nodes.push_back(child.get());
                }
            }
            return nodes;
        }

        /// The 2D bodies a node owns, in the order WorldSnapshot::Object::Body2D lists them: its Rigidbody2D's, then each
        /// Collider2D's own (a collider on the Rigidbody2D's body adds nothing)
        std::vector<Physics::PhysicsBodyHandle> Bodies2DOf(const GameObject &node)
        {
            std::vector<Physics::PhysicsBodyHandle> out;
            const auto add = [&out](const Physics::PhysicsBodyHandle handle)
            {
                if (handle.IsValid() && std::ranges::find(out, handle) == out.end())
                {
                    out.push_back(handle);
                }
            };
            for (const Physics2D::Rigidbody2D *const body : node.GetComponents<Physics2D::Rigidbody2D>())
            {
                add(body->GetHandle());
            }
            for (const Physics2D::Collider2D *const collider : node.GetComponents<Physics2D::Collider2D>())
            {
                add(collider->GetHandle());
            }
            return out;
        }

        /// Every Rigidbody's body handle in the hierarchy, so a late joiner can create the same ones
        void NoteBodyHandles(GameObject &root, std::vector<WorldSnapshot::Object::BodyHandle> &out)
        {
            const auto note = [&out](const std::uint32_t node, const GameObject &target)
            {
                if (const Physics::Rigidbody *const body = target.GetComponent<Physics::Rigidbody>();
                    body != nullptr && body->GetHandle().IsValid())
                {
                    out.push_back(WorldSnapshot::Object::BodyHandle{node, body->GetHandle()});
                }
            };
            note(0, root);
            // The hierarchy walk only for an object that has children: this runs at every save of a rollback window
            if (root.GetChildCount() > 0)
            {
                std::uint32_t node = 1;
                for (const GameObject::Ptr &child : root.GetChildrenRecursive())
                {
                    if (child)
                    {
                        note(node, *child);
                    }
                    ++node;
                }
            }
        }

        /// Every 2D body in the hierarchy, with the pose and velocities a joiner that cannot take the Box2D image falls
        /// back on
        void NoteBodies2D(GameObject &root, std::vector<WorldSnapshot::Object::Body2D> &out)
        {
            const std::vector<GameObject *> nodes = NodesOf(root);
            for (std::uint32_t node = 0; node < nodes.size(); ++node)
            {
                if (nodes[node] == nullptr)
                {
                    continue;
                }
                const GameObject &target = *nodes[node];
                const Positionable *const positionable = target.GetPositionable();
                const Physics2D::Rigidbody2D *const rigidbody = target.GetComponent<Physics2D::Rigidbody2D>();
                for (const Physics::PhysicsBodyHandle handle : Bodies2DOf(target))
                {
                    WorldSnapshot::Object::Body2D body;
                    body.node = node;
                    body.handle = handle;
                    if (positionable != nullptr)
                    {
                        body.position = positionable->GetPosition();
                        body.rotation = positionable->GetRotation();
                    }
                    if (rigidbody != nullptr && rigidbody->GetHandle() == handle)
                    {
                        const Math::Vector2 velocity = rigidbody->GetLinearVelocity();
                        body.linearVelocityX = velocity.x;
                        body.linearVelocityY = velocity.y;
                        body.angularVelocity = rigidbody->GetAngularVelocity();
                    }
                    out.push_back(body);
                }
            }
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
        /// 2: each object carries its bodies' handles (a late joiner creates the same ones). 3: the 2D (Box2D) part and
        /// each object's 2D bodies
        constexpr std::uint32_t kSnapshotFormat = 3;
        /// The most body handles one object's record carries (a hierarchy with more Rigidbodies is cut there; the rest
        /// get handles as usual on a joiner)
        constexpr std::size_t kMaxBodyHandlesPerObject = 1024;
        /// The same for 2D bodies
        constexpr std::size_t kMaxBodies2DPerObject = 1024;
        /// Bytes of one Body2D record: node, handle (2), position (3), rotation (4), velocities (3)
        constexpr std::size_t kBody2DBytes = 4 * 13;

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

        void WriteVector(Writer &writer, const Math::Vector3 &value);

        /// An object's bodies' handles and its 2D bodies (format 3), each list cut at its cap
        void WriteBodies(Writer &writer, const std::vector<WorldSnapshot::Object::BodyHandle> &handles,
                         const std::vector<WorldSnapshot::Object::Body2D> &bodies2d)
        {
            const std::size_t count = std::min<std::size_t>(handles.size(), kMaxBodyHandlesPerObject);
            writer.U16(static_cast<std::uint16_t>(count));
            for (std::size_t i = 0; i < count; ++i)
            {
                writer.U32(handles[i].node);
                writer.U32(handles[i].handle.index);
                writer.U32(handles[i].handle.generation);
            }
            const std::size_t count2D = std::min<std::size_t>(bodies2d.size(), kMaxBodies2DPerObject);
            writer.U16(static_cast<std::uint16_t>(count2D));
            for (std::size_t i = 0; i < count2D; ++i)
            {
                const WorldSnapshot::Object::Body2D &body = bodies2d[i];
                writer.U32(body.node);
                writer.U32(body.handle.index);
                writer.U32(body.handle.generation);
                WriteVector(writer, body.position);
                writer.F32(body.rotation.GetW());
                writer.F32(body.rotation.GetX());
                writer.F32(body.rotation.GetY());
                writer.F32(body.rotation.GetZ());
                writer.F32(body.linearVelocityX);
                writer.F32(body.linearVelocityY);
                writer.F32(body.angularVelocity);
            }
        }

        Math::Vector3 ReadVector(Reader &reader);

        /// The inverse of WriteBodies; the error names what is wrong
        std::expected<void, std::string> ReadBodies(Reader &reader, const std::uint32_t id,
                                                    std::vector<WorldSnapshot::Object::BodyHandle> &handles,
                                                    std::vector<WorldSnapshot::Object::Body2D> &bodies2d)
        {
            const std::uint16_t count = reader.U16();
            if (!reader.Ok() || static_cast<std::size_t>(count) > kMaxBodyHandlesPerObject ||
                static_cast<std::size_t>(count) > reader.Remaining() / 12)
            {
                return std::unexpected(std::format("net object {}: its body handle count is impossible", id));
            }
            for (std::uint16_t h = 0; h < count; ++h)
            {
                WorldSnapshot::Object::BodyHandle record;
                record.node = reader.U32();
                record.handle.index = reader.U32();
                record.handle.generation = reader.U32();
                handles.push_back(record);
            }
            const std::uint16_t count2D = reader.U16();
            if (!reader.Ok() || static_cast<std::size_t>(count2D) > kMaxBodies2DPerObject ||
                static_cast<std::size_t>(count2D) > reader.Remaining() / kBody2DBytes)
            {
                return std::unexpected(std::format("net object {}: its 2D body count is impossible", id));
            }
            for (std::uint16_t b = 0; b < count2D; ++b)
            {
                WorldSnapshot::Object::Body2D body;
                body.node = reader.U32();
                body.handle.index = reader.U32();
                body.handle.generation = reader.U32();
                body.position = ReadVector(reader);
                const float w = reader.F32();
                const float x = reader.F32();
                const float y = reader.F32();
                const float z = reader.F32();
                body.rotation = Math::Quaternion(w, x, y, z);
                body.linearVelocityX = reader.F32();
                body.linearVelocityY = reader.F32();
                body.angularVelocity = reader.F32();
                if (b > 0 && body.node < bodies2d.back().node)
                {
                    return std::unexpected(std::format("net object {}: its 2D bodies are not in node order", id));
                }
                bodies2d.push_back(body);
            }
            return {};
        }

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
        const bool has2D = Live2DWorld(_world) != nullptr;
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
                NoteBodyHandles(gameObject, saved.bodyHandles);
            }
            if (has2D)
            {
                NoteBodies2D(gameObject, saved.bodies2d);
            }
            out.objects.push_back(std::move(saved));
        }
        out.parked.clear();
        for (const auto &entry : _world.Parked())
        {
            NetworkObject &object = *entry.second.object;
            GameObject &gameObject = object.GetGameObject();
            WorldSnapshot::Parked saved;
            saved.id = entry.first;
            saved.owner = object.GetOwner();
            saved.spawned = object.IsSpawned();
            saved.prefab = object.GetPrefabName();
            saved.reason = entry.second.reason;
            saved.tick = entry.second.tick;
            if (_physics != nullptr)
            {
                NoteBodyHandles(gameObject, saved.bodyHandles);
            }
            if (has2D)
            {
                NoteBodies2D(gameObject, saved.bodies2d);
            }
            out.parked.push_back(std::move(saved));
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
        out.hasPhysics2d = false;
        out.physics2d.clear();
        if (Physics2D::Physics2DWorld *const world2D = Live2DWorld(_world); world2D != nullptr)
        {
            if (!world2D->SaveState(out.physics2d))
            {
                _lastError = "the 2D physics world refused SaveState (see the log)";
                return false;
            }
            out.hasPhysics2d = true;
        }
        ++_stats.saves;
        return true;
    }

    std::expected<void, std::string> RollbackWorld::Restore(const WorldSnapshot &snapshot)
    {
        return RestoreImpl(snapshot, true);
    }

    std::expected<void, std::string> RollbackWorld::RestoreImpl(const WorldSnapshot &snapshot, const bool with2D)
    {
        if (snapshot.hasPhysics && !PhysicsActive())
        {
            return std::unexpected(std::string("the snapshot has a physics part but this world has no snapshot-capable backend"));
        }

        Physics2D::Physics2DWorld *const world2D = Live2DWorld(_world);
        if (with2D && snapshot.hasPhysics2d && world2D == nullptr)
        {
            return std::unexpected(std::string("the snapshot has a 2D physics part but this scene has no 2D world"));
        }
        if (with2D && !snapshot.hasPhysics2d && world2D != nullptr)
        {
            return std::unexpected(std::string("the scene has a 2D physics world that did not exist when the snapshot was "
                                               "saved: it cannot be rewound to a state without it. Create the 2D world "
                                               "before the session starts"));
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
        if (with2D && snapshot.hasPhysics2d)
        {
            // Last, like the 3D part: it writes the bodies' poses back to their transforms
            if (!world2D->RestoreState(std::span<const std::byte>(snapshot.physics2d.data(), snapshot.physics2d.size())))
            {
                return std::unexpected(std::string("the 2D physics world refused RestoreState (see the log)"));
            }
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
            const Scene *const scene = _world.GetScene();
            const bool lost2D = scene != nullptr && scene->GetPhysics2DWorld() != nullptr &&
                                HasLost2DBody(object.GetGameObject());
            const bool reusable = object.IsSpawned() && object.GetPrefabName() == prefab && object.GetOwner() == owner &&
                                  pristine != _pristine.end() && !lost2D &&
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
        if (!snapshot.hasPhysics2d && scene->GetPhysics2DWorld() != nullptr)
        {
            return std::unexpected(std::string("this scene has a 2D physics world and the snapshot has none (the other peer "
                                               "made its scene without one): it is not this scene"));
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

        std::map<std::uint32_t, const WorldSnapshot::Parked *> parkedThere;
        for (const WorldSnapshot::Parked &parked : snapshot.parked)
        {
            if (!wanted.contains(parked.id))
            {
                parkedThere.emplace(parked.id, &parked);
            }
        }

        // What this world has and the snapshot does not hold live: parked there (parked here the same way, so its bodies
        // stay, disabled, as on the other peer) or gone there (a despawn released since, a destroyed scene object)
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
            if (const auto there = parkedThere.find(id); there != parkedThere.end())
            {
                (void)_world.Park(NetworkId{id}, there->second->reason, there->second->tick);
                ++result.parked;
            }
            else
            {
                (void)_world.Despawn(NetworkId{id});
            }
            ++result.removed;
        }
        std::vector<std::uint32_t> parked;
        for (const auto &entry : _world.Parked())
        {
            if (const auto there = parkedThere.find(entry.first); there != parkedThere.end())
            {
                // Parked here too: with the other peer's reason and tick, so both release it at the same tick
                (void)_world.Repark(NetworkId{entry.first}, there->second->reason, there->second->tick);
            }
            else if (!wanted.contains(entry.first))
            {
                parked.push_back(entry.first);
            }
        }
        for (const std::uint32_t id : parked)
        {
            _world.ReleaseParked(NetworkId{id});
            ++_stats.released;
        }
        // Their bodies go now, before the spawns below make theirs: a slot a body of the snapshot needs may be one of them
        scene->ProcessDestroyed();

        // What the snapshot has and this world does not: spawned on the other peers before the snapshot. Each of its
        // Rigidbodies' bodies is to be created under the handle it has in the snapshot.
        std::vector<Physics::BodyHandleClaim> claims;
        const auto instantiate = [this, &claims](const std::uint32_t id, const std::string &prefab, const ClientId owner,
                                                 const std::vector<WorldSnapshot::Object::BodyHandle> &handles)
            -> std::expected<void, std::string>
        {
            const auto spawned = _world.SpawnInstance(NetworkId{id}, prefab, owner);
            if (!spawned.has_value())
            {
                return std::unexpected(
                    std::format("net object {} (prefab {}) could not be instantiated: {}", id, prefab, spawned.error()));
            }
            // What Spawn keeps for a fresh instance, so a later spawn of this id may take it back from the pool here as it
            // does on the other peer (an object parked as SpawnUndone, for one)
            GameObject &made = (*spawned)->GetGameObject();
            Pristine pristine;
            pristine.active = made.IsActive();
            pristine.values = CaptureObjectValues(_layouts.Get(NetworkId{id}, made), made);
            if (const Positionable *const positionable = made.GetPositionable(); positionable != nullptr)
            {
                pristine.transform = positionable->GetLocalTransform();
            }
            pristine.descendants = DescendantTransforms(made);
            _pristine[id] = std::move(pristine);
            if (_physics == nullptr || handles.empty())
            {
                return {};
            }
            const std::vector<GameObject *> nodes = NodesOf((*spawned)->GetGameObject());
            for (const WorldSnapshot::Object::BodyHandle &body : handles)
            {
                GameObject *const node = body.node < nodes.size() ? nodes[body.node] : nullptr;
                const Physics::Rigidbody *const rigidbody = node != nullptr ? node->GetComponent<Physics::Rigidbody>() : nullptr;
                if (rigidbody != nullptr)
                {
                    claims.push_back(Physics::BodyHandleClaim{rigidbody, body.handle});
                }
            }
            return {};
        };
        for (const WorldSnapshot::Object &saved : snapshot.objects)
        {
            if (_world.Find(NetworkId{saved.id}) != nullptr || _world.FindParked(NetworkId{saved.id}) != nullptr)
            {
                continue;
            }
            if (auto made = instantiate(saved.id, saved.prefab, saved.owner, saved.bodyHandles); !made.has_value())
            {
                return std::unexpected(made.error());
            }
            ++result.spawned;
        }
        // The spawned objects parked there are made too, and parked once their bodies exist (a scene object parked there
        // and missing here was destroyed here: there is nothing to make it from)
        std::vector<const WorldSnapshot::Parked *> toPark;
        for (const auto &[id, parked] : parkedThere)
        {
            if (!parked->spawned || _world.Find(NetworkId{id}) != nullptr || _world.FindParked(NetworkId{id}) != nullptr)
            {
                continue;
            }
            if (auto made = instantiate(id, parked->prefab, parked->owner, parked->bodyHandles); !made.has_value())
            {
                return std::unexpected(made.error());
            }
            toPark.push_back(parked);
        }
        // Their bodies come into existence here
        result.bodiesClaimed = claims.size();
        if (!claims.empty())
        {
            _physics->ClaimBodyHandles(claims);
        }
        scene->ProcessAttachQueue();
        if (!claims.empty())
        {
            _physics->ClaimBodyHandles(std::span<const Physics::BodyHandleClaim>{});
        }
        for (const WorldSnapshot::Parked *const parked : toPark)
        {
            (void)_world.Park(NetworkId{parked->id}, parked->reason, parked->tick);
            ++result.parked;
        }
        scene->ProcessDestroyed();

        // The 2D world is the scene's or was made when the instantiated objects' 2D components attached above, so it is
        // checked again here (the world has changed by now: a failed join leaves it changed, and the session ends)
        Physics2D::Physics2DWorld *const world2D = Live2DWorld(_world);
        if (snapshot.hasPhysics2d && world2D == nullptr)
        {
            return std::unexpected(std::string("the snapshot has a 2D physics part but this scene has no 2D world"));
        }
        if (!snapshot.hasPhysics2d && world2D != nullptr)
        {
            return std::unexpected(std::string("the objects of the snapshot made a 2D physics world here and the snapshot "
                                               "has none (the other peer's build or scene has no 2D world)"));
        }

        WorldSnapshot objectsOnly = snapshot;
        objectsOnly.hasPhysics = false;
        objectsOnly.physics.clear();
        if (const auto restored = RestoreImpl(objectsOnly, false); !restored.has_value())
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

        if (snapshot.hasPhysics2d)
        {
            // Last, as in Restore: it writes the 2D bodies' poses to their transforms. The bodies of the networked objects
            // take the handles the snapshot gives them, matched by net id, node and order (the two peers may have made them
            // in another order); the rest keep theirs (a scene object made the same way on both).
            std::vector<Physics2D::Physics2DWorld::BodyRename> renames;
            const auto rename = [this, &renames](const std::uint32_t id, const std::vector<WorldSnapshot::Object::Body2D> &bodies)
            {
                NetworkObject *object = _world.Find(NetworkId{id});
                if (object == nullptr)
                {
                    const NetworkWorld::ParkedObject *const parked = _world.FindParked(NetworkId{id});
                    object = parked != nullptr ? parked->object : nullptr;
                }
                if (object == nullptr || bodies.empty())
                {
                    return;
                }
                const std::vector<GameObject *> nodes = NodesOf(object->GetGameObject());
                std::size_t at = 0;
                while (at < bodies.size())
                {
                    const std::uint32_t node = bodies[at].node;
                    const std::vector<Physics::PhysicsBodyHandle> here = node < nodes.size() && nodes[node] != nullptr
                                                                             ? Bodies2DOf(*nodes[node])
                                                                             : std::vector<Physics::PhysicsBodyHandle>{};
                    for (std::size_t k = 0; at < bodies.size() && bodies[at].node == node; ++k, ++at)
                    {
                        if (k < here.size())
                        {
                            renames.push_back(Physics2D::Physics2DWorld::BodyRename{here[k], bodies[at].handle});
                        }
                    }
                }
            };
            for (const WorldSnapshot::Object &saved : snapshot.objects)
            {
                rename(saved.id, saved.bodies2d);
            }
            for (const WorldSnapshot::Parked &saved : snapshot.parked)
            {
                rename(saved.id, saved.bodies2d);
            }
            // Every 2D node's pose as saved: transforms are not replicated values, and the restore below writes back only
            // the poses of enabled dynamic bodies, so a static or disabled body's object (a wall the game moved after
            // spawning it, a parked shot) would keep the pose its prefab gave it here
            const auto place = [this](const std::uint32_t id, const std::vector<WorldSnapshot::Object::Body2D> &bodies)
            {
                NetworkObject *object = _world.Find(NetworkId{id});
                if (object == nullptr)
                {
                    const NetworkWorld::ParkedObject *const parked = _world.FindParked(NetworkId{id});
                    object = parked != nullptr ? parked->object : nullptr;
                }
                if (object == nullptr || bodies.empty())
                {
                    return;
                }
                const std::vector<GameObject *> nodes = NodesOf(object->GetGameObject());
                std::set<std::uint32_t> placed;
                for (const WorldSnapshot::Object::Body2D &body : bodies)
                {
                    GameObject *const node = body.node < nodes.size() ? nodes[body.node] : nullptr;
                    if (node == nullptr || !placed.insert(body.node).second)
                    {
                        continue; // a node's first body carries its pose
                    }
                    if (Positionable *const positionable = node->GetPositionable(); positionable != nullptr)
                    {
                        positionable->SetPositionAndRotation(body.position, body.rotation);
                    }
                }
            };
            for (const WorldSnapshot::Object &saved : snapshot.objects)
            {
                place(saved.id, saved.bodies2d);
            }
            for (const WorldSnapshot::Parked &saved : snapshot.parked)
            {
                place(saved.id, saved.bodies2d);
            }

            const Physics2D::Physics2DWorld::JoinRestore2D joined = world2D->RestoreStateForJoin(
                std::span<const std::byte>(snapshot.physics2d.data(), snapshot.physics2d.size()), renames);
            using Status = Physics2D::Physics2DWorld::JoinRestore2D::Status;
            if (joined.status == Status::Broken)
            {
                return std::unexpected(std::string("the 2D physics world refused the snapshot's 2D part after the restore "
                                                   "began (see the log); this world must not be used"));
            }
            result.lost2d = joined.lost;
            result.removed2d = joined.removed;
            if (joined.status == Status::Restored)
            {
                result.exactPhysics2d = joined.lost == 0 && joined.removed == 0;
            }
            else
            {
                // This build cannot read the image: the networked objects' 2D bodies are where they were (placed above)
                // and get their speeds
                result.image2dRefused = true;
                for (const WorldSnapshot::Object &saved : snapshot.objects)
                {
                    NetworkObject *const object = _world.Find(NetworkId{saved.id});
                    if (object == nullptr)
                    {
                        continue;
                    }
                    const std::vector<GameObject *> nodes = NodesOf(object->GetGameObject());
                    std::set<std::uint32_t> placed;
                    for (const WorldSnapshot::Object::Body2D &body : saved.bodies2d)
                    {
                        GameObject *const node = body.node < nodes.size() ? nodes[body.node] : nullptr;
                        if (node == nullptr || !placed.insert(body.node).second)
                        {
                            continue; // a node's first body carries its Rigidbody2D's velocities
                        }
                        if (Physics2D::Rigidbody2D *const rigidbody = node->GetComponent<Physics2D::Rigidbody2D>();
                            rigidbody != nullptr && rigidbody->GetHandle().IsValid())
                        {
                            rigidbody->SetLinearVelocity(Math::Vector2(body.linearVelocityX, body.linearVelocityY));
                            rigidbody->SetAngularVelocity(body.angularVelocity);
                        }
                    }
                }
            }
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
                writer.F32(body.rotation.GetW());
                writer.F32(body.rotation.GetX());
                writer.F32(body.rotation.GetY());
                writer.F32(body.rotation.GetZ());
                WriteVector(writer, body.linearVelocity);
                WriteVector(writer, body.angularVelocity);
                writer.U8(body.sleeping ? 1 : 0);
            }
            WriteBodies(writer, object.bodyHandles, object.bodies2d);
        }
        writer.U32(static_cast<std::uint32_t>(snapshot.parked.size()));
        for (const WorldSnapshot::Parked &parked : snapshot.parked)
        {
            writer.U32(parked.id);
            writer.U32(parked.owner);
            writer.U8(parked.spawned ? 1 : 0);
            writer.U8(static_cast<std::uint8_t>(parked.reason));
            writer.U32(parked.tick);
            const std::size_t prefabBytes = std::min<std::size_t>(parked.prefab.size(), 0xFFFFu);
            writer.U16(static_cast<std::uint16_t>(prefabBytes));
            writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(parked.prefab.data()), prefabBytes));
            WriteBodies(writer, parked.bodyHandles, parked.bodies2d);
        }
        writer.U8(snapshot.hasPhysics ? 1 : 0);
        writer.U32(static_cast<std::uint32_t>(snapshot.physics.size()));
        writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(snapshot.physics.data()),
                                                   snapshot.physics.size()));
        writer.U8(snapshot.hasPhysics2d ? 1 : 0);
        writer.U32(static_cast<std::uint32_t>(snapshot.physics2d.size()));
        writer.Bytes(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(snapshot.physics2d.data()),
                                                   snapshot.physics2d.size()));
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
            if (auto bodies = ReadBodies(reader, object.id, object.bodyHandles, object.bodies2d); !bodies.has_value())
            {
                return std::unexpected(bodies.error());
            }
            snapshot.objects.push_back(std::move(object));
        }
        const std::uint32_t parkedCount = reader.U32();
        if (!reader.Ok() || parkedCount > kMaxNetworkObjects)
        {
            return std::unexpected(std::string("the snapshot holds more parked objects than kMaxNetworkObjects"));
        }
        std::set<std::uint32_t> live;
        for (const WorldSnapshot::Object &object : snapshot.objects)
        {
            live.insert(object.id);
        }
        for (std::uint32_t i = 0; i < parkedCount; ++i)
        {
            WorldSnapshot::Parked parked;
            parked.id = reader.U32();
            parked.owner = reader.U32();
            parked.spawned = reader.U8() != 0;
            const std::uint8_t reason = reader.U8();
            parked.tick = reader.U32();
            const std::uint16_t prefabBytes = reader.U16();
            const std::span<const std::uint8_t> prefab = reader.Bytes(prefabBytes);
            if (!reader.Ok())
            {
                return std::unexpected(std::format("the snapshot is cut short in parked object {}", i));
            }
            if (parked.id == 0 || (i > 0 && parked.id <= snapshot.parked.back().id) || live.contains(parked.id))
            {
                return std::unexpected(std::string("the snapshot's parked objects are not in ascending id order, or one is "
                                                   "also live"));
            }
            if (reason > static_cast<std::uint8_t>(NetworkWorld::ParkReason::SpawnUndone))
            {
                return std::unexpected(std::format("parked object {}: unknown park reason {}", parked.id, reason));
            }
            parked.reason = static_cast<NetworkWorld::ParkReason>(reason);
            parked.prefab.assign(reinterpret_cast<const char *>(prefab.data()), prefab.size());
            if (auto bodies = ReadBodies(reader, parked.id, parked.bodyHandles, parked.bodies2d); !bodies.has_value())
            {
                return std::unexpected(bodies.error());
            }
            snapshot.parked.push_back(std::move(parked));
        }
        snapshot.hasPhysics = reader.U8() != 0;
        const std::uint32_t physicsBytes = reader.U32();
        const std::span<const std::uint8_t> physics = reader.Bytes(physicsBytes);
        snapshot.hasPhysics2d = reader.U8() != 0;
        const std::uint32_t physics2dBytes = reader.U32();
        const std::span<const std::uint8_t> physics2d = reader.Bytes(physics2dBytes);
        if (!reader.Ok() || reader.Remaining() != 0)
        {
            return std::unexpected(std::string("the snapshot is cut short or has bytes after its 2D physics part"));
        }
        if (snapshot.hasPhysics2d == physics2d.empty())
        {
            return std::unexpected(std::string("the snapshot's 2D physics flag does not match its 2D physics bytes"));
        }
        snapshot.physics.resize(physics.size());
        if (!physics.empty())
        {
            std::memcpy(snapshot.physics.data(), physics.data(), physics.size());
        }
        snapshot.physics2d.resize(physics2d.size());
        if (!physics2d.empty())
        {
            std::memcpy(snapshot.physics2d.data(), physics2d.data(), physics2d.size());
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
