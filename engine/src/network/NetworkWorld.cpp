#include "engine/network/NetworkWorld.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <vector>

#include "engine/GameObjectScene.hpp"
#include "engine/prefabs/PrefabManager.hpp"

namespace N2Engine::Network
{
    namespace
    {
        /// NetworkObjects in the whole tree under `root`, `root` included
        std::size_t CountNetworkObjects(GameObject &root)
        {
            std::size_t count = 0;
            // A stack on the heap: the hierarchy is bounded by JsonLimits, but there is no need to recurse
            std::vector<GameObject *> pending{&root};
            while (!pending.empty())
            {
                GameObject *const gameObject = pending.back();
                pending.pop_back();
                count += gameObject->GetComponents<NetworkObject>().size();
                for (const GameObject::Ptr &child : gameObject->GetChildren())
                {
                    pending.push_back(child.get());
                }
            }
            return count;
        }
    }

    NetworkWorld::~NetworkWorld()
    {
        Reset();
    }

    std::expected<std::uint32_t, std::string> NetworkWorld::BindScene(Scene &scene)
    {
        Reset();

        struct Candidate
        {
            std::array<std::uint8_t, 16> key{};
            NetworkObject *object = nullptr;
        };
        std::vector<Candidate> candidates;
        bool tooMany = false;
        bool taken = false;
        scene.TraverseAll(
            [&](const std::shared_ptr<GameObject> &gameObject)
            {
                if (gameObject->IsDestroyed())
                {
                    return;
                }
                NetworkObject *const object = gameObject->GetComponent<NetworkObject>();
                if (object == nullptr)
                {
                    return;
                }
                if (object->IsRegistered())
                {
                    taken = true;
                    return;
                }
                if (candidates.size() >= kMaxNetworkObjects)
                {
                    tooMany = true;
                    return;
                }
                candidates.push_back(Candidate{gameObject->GetUUID().GetBytes(), object});
            });
        if (taken)
        {
            return std::unexpected(std::string("a NetworkObject in the scene already belongs to another network world"));
        }
        if (tooMany)
        {
            return std::unexpected(std::format("the scene has more than {} NetworkObjects", kMaxNetworkObjects));
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate &a, const Candidate &b) { return a.key < b.key; });
        for (std::size_t i = 1; i < candidates.size(); ++i)
        {
            if (candidates[i - 1].key == candidates[i].key)
            {
                const GameObject &first = candidates[i - 1].object->GetGameObject();
                const GameObject &second = candidates[i].object->GetGameObject();
                return std::unexpected(std::format("two NetworkObjects share the GameObject UUID {} ('{}' and '{}'): scene "
                                                   "objects are matched by UUID, so every one needs its own",
                                                   first.GetUUID().ToString(), first.GetName(), second.GetName()));
            }
        }

        std::uint32_t next = 1;
        for (const Candidate &candidate : candidates)
        {
            NetworkObject &object = *candidate.object;
            object._world = this;
            object._id = NetworkId{next};
            object._owner = kServerClientId;
            object._spawned = false;
            object._prefabName.clear();
            _objects.emplace(next, &object);
            ++next;
        }
        _scene = &scene;
        _sceneLifetime = scene.GetLifetimeToken();
        _sceneObjectCount = static_cast<std::uint32_t>(candidates.size());
        _nextId = next;
        return _sceneObjectCount;
    }

    void NetworkWorld::Reset()
    {
        for (auto &entry : _parked)
        {
            NetworkObject &object = *entry.second.object;
            object._world = nullptr;
            object._id = NetworkId{};
            object._owner = kServerClientId;
            object._spawned = false;
            object._prefabName.clear();
            object.GetGameObject().SetActive(entry.second.wasActive);
        }
        _parked.clear();
        for (auto &entry : _objects)
        {
            NetworkObject &object = *entry.second;
            object._world = nullptr;
            object._id = NetworkId{};
            object._owner = kServerClientId;
            object._spawned = false;
            object._prefabName.clear();
        }
        _objects.clear();
        _scene = nullptr;
        _sceneLifetime.reset();
        _sceneObjectCount = 0;
        _nextId = 1;
    }

    bool NetworkWorld::HasScene() const noexcept
    {
        return _scene != nullptr && !_sceneLifetime.expired();
    }

    Scene *NetworkWorld::GetScene() const noexcept
    {
        return HasScene() ? _scene : nullptr;
    }

    NetworkObject *NetworkWorld::Find(const NetworkId id) const
    {
        const auto found = _objects.find(id.value);
        return found == _objects.end() ? nullptr : found->second;
    }

    NetworkId NetworkWorld::AllocateId()
    {
        if (_nextId == 0 || _nextId == std::numeric_limits<std::uint32_t>::max())
        {
            return NetworkId{};
        }
        const NetworkId id{_nextId};
        ++_nextId;
        return id;
    }

    std::expected<NetworkObject *, std::string> NetworkWorld::SpawnInstance(const NetworkId id,
                                                                            const std::string_view prefabName,
                                                                            const ClientId owner)
    {
        Scene *const scene = GetScene();
        if (scene == nullptr)
        {
            return std::unexpected(std::string("the network world has no scene"));
        }
        if (!id.IsValid())
        {
            return std::unexpected(std::string("net id 0 is not a valid id"));
        }
        if (_objects.contains(id.value) || _parked.contains(id.value))
        {
            return std::unexpected(std::format("net id {} is already in use", id.value));
        }
        if (_objects.size() >= kMaxNetworkObjects)
        {
            return std::unexpected(std::format("the world already holds {} network objects, the most there can be",
                                               kMaxNetworkObjects));
        }

        const std::string name(prefabName);
        std::shared_ptr<GameObject> instance = PrefabManager::InstantiateRegisteredPrefab(name);
        if (!instance)
        {
            return std::unexpected(std::format("prefab '{}' could not be instantiated (see the log)", name));
        }
        NetworkObject *const object = instance->GetComponent<NetworkObject>();
        if (object == nullptr || CountNetworkObjects(*instance) != 1)
        {
            // The instance is in no scene yet, so dropping it frees it with nothing attached
            return std::unexpected(std::format("prefab '{}' must have exactly one NetworkObject, on its root object", name));
        }

        scene->AddRootGameObject(instance);
        object->_world = this;
        object->_id = id;
        object->_owner = owner;
        object->_spawned = true;
        object->_prefabName = name;
        _objects.emplace(id.value, object);
        return object;
    }

    NetworkWorld::DespawnResult NetworkWorld::Despawn(const NetworkId id)
    {
        const auto found = _objects.find(id.value);
        if (found == _objects.end())
        {
            return {};
        }
        NetworkObject *const object = found->second;
        DespawnResult result;
        result.found = true;
        result.wasSpawned = object->_spawned;

        _objects.erase(found);
        object->_world = nullptr;
        object->_id = NetworkId{};
        object->_owner = kServerClientId;
        object->_spawned = false;
        object->_prefabName.clear();
        object->GetGameObject().Destroy();
        return result;
    }

    bool NetworkWorld::Park(const NetworkId id, const ParkReason reason, const std::uint32_t tick)
    {
        const auto found = _objects.find(id.value);
        if (found == _objects.end())
        {
            return false;
        }
        NetworkObject *const object = found->second;
        _objects.erase(found);
        ParkedObject parked;
        parked.object = object;
        parked.reason = reason;
        parked.tick = tick;
        parked.wasActive = object->GetGameObject().IsActive();
        _parked.emplace(id.value, parked);
        object->GetGameObject().SetActive(false);
        return true;
    }

    NetworkObject *NetworkWorld::Unpark(const NetworkId id, const bool reactivate)
    {
        const auto found = _parked.find(id.value);
        if (found == _parked.end())
        {
            return nullptr;
        }
        const ParkedObject parked = found->second;
        _parked.erase(found);
        _objects.emplace(id.value, parked.object);
        if (reactivate)
        {
            parked.object->GetGameObject().SetActive(parked.wasActive);
        }
        return parked.object;
    }

    const NetworkWorld::ParkedObject *NetworkWorld::FindParked(const NetworkId id) const
    {
        const auto found = _parked.find(id.value);
        return found == _parked.end() ? nullptr : &found->second;
    }

    bool NetworkWorld::ReleaseParked(const NetworkId id)
    {
        const auto found = _parked.find(id.value);
        if (found == _parked.end())
        {
            return false;
        }
        NetworkObject *const object = found->second.object;
        _parked.erase(found);
        object->_world = nullptr;
        object->_id = NetworkId{};
        object->_owner = kServerClientId;
        object->_spawned = false;
        object->_prefabName.clear();
        // Inactive already, so its body (if any) is disabled before the destroy pass removes it
        object->GetGameObject().Destroy();
        return true;
    }

    void NetworkWorld::ObjectGone(NetworkObject &object)
    {
        const NetworkId id = object._id;
        const bool wasSpawned = object._spawned;
        if (id.IsValid())
        {
            const auto found = _objects.find(id.value);
            if (found != _objects.end() && found->second == &object)
            {
                _objects.erase(found);
            }
            const auto parked = _parked.find(id.value);
            if (parked != _parked.end() && parked->second.object == &object)
            {
                _parked.erase(parked);
            }
        }
        object._id = NetworkId{};
        if (id.IsValid() && _destroyedHandler)
        {
            _destroyedHandler(id, wasSpawned);
        }
    }
}
