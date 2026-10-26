#include "engine/network/RollbackWorld.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <span>
#include <vector>

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
        /// True when anything UNDER the root has a Rigidbody. A Rigidbody on the root itself is handled (its body is reset
        /// to the spawn pose when the object is pooled); one on a child is not, and such an object is never pooled.
        bool HasNestedRigidbody(GameObject &root)
        {
            std::vector<GameObject *> pending;
            for (const GameObject::Ptr &child : root.GetChildren())
            {
                pending.push_back(child.get());
            }
            while (!pending.empty())
            {
                GameObject *const gameObject = pending.back();
                pending.pop_back();
                if (gameObject->GetComponent<Physics::Rigidbody>() != nullptr)
                {
                    return true;
                }
                for (const GameObject::Ptr &child : gameObject->GetChildren())
                {
                    pending.push_back(child.get());
                }
            }
            return false;
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
            const bool pooled = object->IsSpawned() && !HasNestedRigidbody(object->GetGameObject());
            (void)_world.Park(NetworkId{id}, NetworkWorld::ParkReason::SpawnUndone, snapshot.tick);
            if (!pooled)
            {
                // A body that did not exist at the snapshot may go: it is disabled by the park, destroyed with the object
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
            const bool reusable = object.IsSpawned() && object.GetPrefabName() == prefab && object.GetOwner() == owner &&
                                  pristine != _pristine.end() && !HasNestedRigidbody(object.GetGameObject());
            if (reusable)
            {
                // Back to the state right after instantiation while it is still parked: its transform, its replicated
                // fields, then (as it becomes active again, which enables a Rigidbody's body at the GameObject's pose) its
                // body without the velocity and sleep state of its earlier life
                GameObject &gameObject = object.GetGameObject();
                if (pristine->second.transform.has_value())
                {
                    if (Positionable *const positionable = gameObject.GetPositionable(); positionable != nullptr)
                    {
                        positionable->SetLocalTransform(*pristine->second.transform);
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
                    const bool deferred = gameObject.GetComponent<Physics::Rigidbody>() != nullptr;
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
        Physics::Rigidbody *const body = gameObject.GetComponent<Physics::Rigidbody>();
        const Positionable *const positionable = gameObject.GetPositionable();
        if (_physics == nullptr || body == nullptr || positionable == nullptr || !body->GetHandle().IsValid())
        {
            return;
        }
        Physics::BodyState state;
        state.position = positionable->GetPosition();
        state.rotation = positionable->GetRotation();
        // A static body takes no state (the call is refused and there is nothing to reset)
        (void)_physics->SetBodyState(body->GetHandle(), state);
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
