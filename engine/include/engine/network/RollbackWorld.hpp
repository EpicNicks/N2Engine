#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <math/DeterministicRandom.hpp>

#include "engine/Transform.hpp"

#include "engine/network/NetworkId.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/network/ReplicationLayout.hpp"

namespace N2Engine::Physics
{
    class IPhysicsBackend;
}

namespace N2Engine::Network
{
    /**
     * One saved simulation state (netcode C5, design 4.4): the part of the world a rollback puts back. Made by
     * RollbackWorld::Save, applied by RollbackWorld::Restore. It belongs to the process and the build that made it (the
     * physics part is the backend's own bytes), it is not a wire format.
     */
    struct WorldSnapshot
    {
        struct Object
        {
            std::uint32_t id = 0;
            ClientId owner = kServerClientId;
            bool spawned = false;
            std::string prefab;
            /// The GameObject's own active flag (it decides whether a Rigidbody's body is in the simulation)
            bool active = true;
            /// Every replicated field, unquantised
            ObjectValues values;
        };

        /// The state BEFORE tick `tick` runs (so the world after tick - 1 ticks)
        std::uint32_t tick = 0;
        /// NetworkWorld::NextId at save time
        std::uint32_t nextId = 0;
        /// The live net objects, ascending by id
        std::vector<Object> objects;
        /// IPhysicsBackend::SaveState bytes; empty when the world has no snapshot-capable backend
        std::vector<std::byte> physics;
        bool hasPhysics = false;
        /// The global seeded RNG stream (math/Random.hpp); nullopt when it was not seeded
        std::optional<std::uint32_t> rng;
        /// The registered DeterministicRandom streams (RollbackWorld::AddRandomStream), in registration order
        std::vector<std::uint32_t> streams;
    };

    struct RollbackWorldStats
    {
        std::uint64_t saves = 0;
        std::uint64_t restores = 0;
        /// Objects parked by a restore because they did not exist at the restore point
        std::uint64_t spawnsUndone = 0;
        /// Objects brought back by a restore because they were despawned after the restore point
        std::uint64_t despawnsUndone = 0;
        /// Spawns that took a parked object instead of instantiating a prefab
        std::uint64_t spawnsPooled = 0;
        /// Parked objects destroyed for good
        std::uint64_t released = 0;
    };

    /**
     * The rollback world contract (design 4.4) applied to one NetworkWorld and, optionally, the physics backend of its
     * scene.
     *
     * SIM STATE = what a snapshot holds: the net id set and each object's owner, spawn flag, prefab, active flag and every
     * replicated field (NetworkTransform included), the id counter, the global seeded RNG stream, and the physics world
     * (IPhysicsBackend::SaveState). Lua script state is part of it only through the script's declared replicated fields
     * (LuaComponent::SerializeReplicated), exactly as the design asks; locals, closures and coroutines are not captured.
     * Everything else (audio, particles, UI, a component's non-replicated fields) is presentation, rebuilt from sim state or
     * left alone.
     *
     * SPAWN AND DESPAWN go through Spawn and Despawn so a rollback can undo them. A despawn PARKS the object (inactive,
     * unregistered, nothing destroyed) until ReleaseConfirmed says no snapshot still needs it. A restore parks the objects
     * that did not exist at the restore point and unparks the ones that did, so a Rigidbody's body keeps its handle and the
     * physics parked-body contract holds (the enabled bodies at Restore are the ones at Save). A resimulation that spawns
     * the same prefab under the same id takes the parked object back (the pool) and resets its transform and replicated
     * fields to what they were right after instantiation, and a Rigidbody on its root to that pose with no velocity (the
     * body keeps its handle, which is what keeps the handle numbering, and so the physics hash, equal to a run that never
     * rolled back). A spawned object with a Rigidbody on a CHILD is not pooled: a restore destroys it (a body that did not
     * exist at the snapshot may be created or destroyed freely) and the resimulation instantiates a fresh one, whose
     * handle can differ from the first run's.
     *
     * Call Save and Restore only at a tick boundary, after a complete step (the physics backend needs its collision
     * callbacks processed, the scene its attach and destroy queues). Restore also calls SyncTransforms on the backend.
     * Single-threaded, like the scene.
     */
    class RollbackWorld
    {
    public:
        /// `physics` may be null (a world with no physics, or a backend that cannot snapshot: then the snapshot has no
        /// physics part and Restore leaves the backend alone)
        RollbackWorld(NetworkWorld &world, Physics::IPhysicsBackend *physics) noexcept;
        RollbackWorld(const RollbackWorld &) = delete;
        RollbackWorld &operator=(const RollbackWorld &) = delete;

        /// Writes the state before tick `tick` into `out` (replacing it). False, with the reason in LastError, when the
        /// physics backend refuses to save.
        [[nodiscard]] bool Save(std::uint32_t tick, WorldSnapshot &out);

        /// Puts a snapshot back. Everything that can be refused is checked first and nothing changes in that case: an
        /// object of the snapshot that is neither registered nor parked (released too early), or a layout that changed.
        /// A failure inside the physics restore (after the objects were rewound) is reported too, and the world is then
        /// not usable: the session must stop.
        [[nodiscard]] std::expected<void, std::string> Restore(const WorldSnapshot &snapshot);

        /// The simulation spawns `prefab` for `owner` at tick `tick`: the next net id, a parked object of that id and
        /// prefab if there is one, else a new instance. Use this instead of NetworkWorld::AllocateId + SpawnInstance in
        /// simulation code under rollback.
        [[nodiscard]] std::expected<NetworkObject *, std::string> Spawn(std::string_view prefab, ClientId owner,
                                                                       std::uint32_t tick);

        /// Activates the pooled objects Spawn handed out inactive (those with a Rigidbody), with their bodies at the pose
        /// the game gave them and no velocity. Call it where the scene's attach queue runs, at the end of the tick and
        /// before Save; a fresh instance gets its body at the same point.
        void CommitSpawns();

        /// The simulation despawns `id` at tick `tick` (parks it). False when it is not registered.
        bool Despawn(NetworkId id, std::uint32_t tick);

        /// Destroys the despawned objects no snapshot at or after `oldestSnapshotTick` can need (parked before that
        /// tick). Returns how many.
        std::size_t ReleaseConfirmed(std::uint32_t oldestSnapshotTick);

        /// Destroys the parked objects a rollback undid the spawn of and the resimulation did not spawn again. Call it
        /// when a resimulation has caught up. Returns how many.
        std::size_t ReleasePool();

        /// Registers a simulation-owned random stream (a game's own DeterministicRandom) so snapshots keep its state. The
        /// stream must outlive this object. Register before the first Save; the global stream (Random::Seed) is always
        /// covered.
        void AddRandomStream(Math::DeterministicRandom &stream) { _streams.push_back(&stream); }

        [[nodiscard]] NetworkWorld &World() noexcept { return _world; }
        [[nodiscard]] const NetworkWorld &World() const noexcept { return _world; }
        [[nodiscard]] const RollbackWorldStats &Stats() const noexcept { return _stats; }
        [[nodiscard]] const std::string &LastError() const noexcept { return _lastError; }
        [[nodiscard]] LayoutCache &Layouts() noexcept { return _layouts; }

    private:
        struct Pristine
        {
            ObjectValues values;
            bool active = true;
            /// The root's local transform right after instantiation (nullopt: it has no Positionable)
            std::optional<Transform> transform;
        };

        [[nodiscard]] bool PhysicsActive() const;
        /// A pooled object's Rigidbody (on the root) goes back to its pose with zero velocities and awake
        void ResetBody(GameObject &gameObject) const;
        void ForgetStalePristine();

        NetworkWorld &_world;
        Physics::IPhysicsBackend *_physics;
        LayoutCache _layouts;
        std::vector<Math::DeterministicRandom *> _streams;
        /// Pooled objects waiting for CommitSpawns
        std::vector<std::uint32_t> _uncommitted;
        /// By net id: the replicated fields of a spawned object right after it was instantiated
        std::map<std::uint32_t, Pristine> _pristine;
        RollbackWorldStats _stats;
        std::string _lastError;
    };
}
