#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "NetworkTestSupport.hpp"
#include "engine/network/NetworkObject.hpp"
#include "engine/network/NetworkWorld.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

// The registry of one scene's NetworkObjects (netcode stage B1): scene-object matching by GameObject UUID, ids that are
// never reused, prefab instantiation and the checks it makes, despawn, and what the world does when objects or the scene
// go away under it.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    /// The net id of the scene object numbered `n`, or 0
    std::uint32_t IdOf(Scene &scene, const std::uint32_t n)
    {
        const GameObject::Ptr object = FindByNumber(scene, n);
        if (!object)
        {
            return 0;
        }
        const NetworkObject *networkObject = object->GetComponent<NetworkObject>();
        return networkObject == nullptr ? 0 : networkObject->GetNetworkId().value;
    }
}

TEST(NetworkWorldTest, NetworkObjectIsARegisteredSingletonComponent)
{
    auto &registry = ComponentRegistry::Instance();
    EXPECT_TRUE(registry.IsRegistered("NetworkObject"));
    EXPECT_TRUE(registry.IsSingleton("NetworkObject"));

    const std::unique_ptr<Scene> scene = MakeScene("singleton", {1});
    const GameObject::Ptr object = FindByNumber(*scene, 1);
    ASSERT_TRUE(object);
    NetworkObject *first = object->GetComponent<NetworkObject>();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(object->AddComponent<NetworkObject>(), first);
    EXPECT_EQ(first->GetTypeName(), "NetworkObject");
    EXPECT_FALSE(first->HasNetworkId());
    EXPECT_FALSE(first->IsRegistered());
    EXPECT_EQ(first->GetOwner(), kServerClientId);
    EXPECT_FALSE(first->IsSpawned());
    EXPECT_TRUE(first->GetPrefabName().empty());
}

TEST(NetworkWorldTest, SceneObjectsGetIdsInTheOrderOfTheirUuids)
{
    const std::unique_ptr<Scene> scene = MakeScene("order", {30, 10, 20});
    NetworkWorld world;
    const auto bound = world.BindScene(*scene);
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*bound, 3u);
    EXPECT_EQ(world.SceneObjectCount(), 3u);
    EXPECT_EQ(world.Count(), 3u);
    EXPECT_TRUE(world.HasScene());
    EXPECT_EQ(world.GetScene(), scene.get());

    EXPECT_EQ(IdOf(*scene, 10), 1u);
    EXPECT_EQ(IdOf(*scene, 20), 2u);
    EXPECT_EQ(IdOf(*scene, 30), 3u);
    for (const std::uint32_t n : {10u, 20u, 30u})
    {
        const NetworkObject *object = FindByNumber(*scene, n)->GetComponent<NetworkObject>();
        EXPECT_TRUE(object->IsRegistered());
        EXPECT_FALSE(object->IsSpawned());
        EXPECT_EQ(object->GetOwner(), kServerClientId);
        EXPECT_EQ(world.Find(object->GetNetworkId()), object);
    }
    EXPECT_EQ(world.Find(NetworkId{0}), nullptr);
    EXPECT_EQ(world.Find(NetworkId{4}), nullptr);
}

TEST(NetworkWorldTest, TwoScenesBuiltInDifferentOrdersAgreeOnEveryId)
{
    const std::unique_ptr<Scene> serverScene = MakeScene("server", {5, 1, 9, 3, 7});
    const std::unique_ptr<Scene> clientScene = MakeScene("client", {9, 3, 7, 5, 1});
    NetworkWorld serverWorld;
    NetworkWorld clientWorld;
    ASSERT_TRUE(serverWorld.BindScene(*serverScene).has_value());
    ASSERT_TRUE(clientWorld.BindScene(*clientScene).has_value());
    for (const std::uint32_t n : {1u, 3u, 5u, 7u, 9u})
    {
        EXPECT_EQ(IdOf(*serverScene, n), IdOf(*clientScene, n)) << "object " << n;
        EXPECT_NE(IdOf(*serverScene, n), 0u);
    }
}

TEST(NetworkWorldTest, ChildrenAndInactiveObjectsAreSceneObjectsToo)
{
    const std::unique_ptr<Scene> scene = MakeScene("nested", {2});
    const GameObject::Ptr parent = FindByNumber(*scene, 2);
    const GameObject::Ptr child = GameObject::Create("Child");
    child->SetUUID(TestUuid(1));
    child->AddComponent<NetworkObject>();
    parent->AddChild(child, false);
    const GameObject::Ptr hidden = GameObject::Create("Hidden");
    hidden->SetUUID(TestUuid(3));
    hidden->AddComponent<NetworkObject>();
    hidden->SetActive(false);
    scene->AddRootGameObject(hidden);
    // An object without a NetworkObject is not one
    const GameObject::Ptr plain = GameObject::Create("Plain");
    plain->SetUUID(TestUuid(4));
    scene->AddRootGameObject(plain);

    NetworkWorld world;
    const auto bound = world.BindScene(*scene);
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*bound, 3u);
    EXPECT_EQ(child->GetComponent<NetworkObject>()->GetNetworkId().value, 1u);
    EXPECT_EQ(parent->GetComponent<NetworkObject>()->GetNetworkId().value, 2u);
    EXPECT_EQ(hidden->GetComponent<NetworkObject>()->GetNetworkId().value, 3u);
}

TEST(NetworkWorldTest, ASceneWithNoNetworkObjectsBindsEmpty)
{
    const std::unique_ptr<Scene> scene = MakeScene("none", {1, 2}, false);
    NetworkWorld world;
    const auto bound = world.BindScene(*scene);
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*bound, 0u);
    EXPECT_EQ(world.Count(), 0u);
    EXPECT_TRUE(world.HasScene());
    EXPECT_EQ(world.AllocateId().value, 1u);
}

TEST(NetworkWorldTest, ADuplicateUuidFailsTheBindAndLeavesNothingRegistered)
{
    const std::unique_ptr<Scene> scene = MakeScene("duplicate", {1, 2});
    const GameObject::Ptr twin = GameObject::Create("Twin");
    twin->SetUUID(TestUuid(2));
    twin->AddComponent<NetworkObject>();
    scene->AddRootGameObject(twin);

    NetworkWorld world;
    const auto bound = world.BindScene(*scene);
    ASSERT_FALSE(bound.has_value());
    EXPECT_NE(bound.error().find("share the GameObject UUID"), std::string::npos);
    EXPECT_EQ(world.Count(), 0u);
    EXPECT_FALSE(world.HasScene());
    EXPECT_EQ(IdOf(*scene, 1), 0u);
    EXPECT_FALSE(twin->GetComponent<NetworkObject>()->IsRegistered());
}

TEST(NetworkWorldTest, MoreThanTheCapFailsTheBind)
{
    const std::unique_ptr<Scene> atCap = MakeScene("at-cap", [] {
        std::vector<std::uint32_t> numbers;
        for (std::uint32_t n = 1; n <= kMaxNetworkObjects; ++n)
        {
            numbers.push_back(n);
        }
        return numbers;
    }());
    NetworkWorld world;
    const auto bound = world.BindScene(*atCap);
    ASSERT_TRUE(bound.has_value());
    EXPECT_EQ(*bound, kMaxNetworkObjects);
    EXPECT_EQ(IdOf(*atCap, kMaxNetworkObjects), kMaxNetworkObjects);

    const GameObject::Ptr extra = GameObject::Create("OneTooMany");
    extra->SetUUID(TestUuid(kMaxNetworkObjects + 1));
    extra->AddComponent<NetworkObject>();
    atCap->AddRootGameObject(extra);
    const auto over = world.BindScene(*atCap);
    ASSERT_FALSE(over.has_value());
    EXPECT_NE(over.error().find("more than"), std::string::npos);
    EXPECT_EQ(world.Count(), 0u);
    EXPECT_EQ(IdOf(*atCap, 1), 0u);
}

TEST(NetworkWorldTest, AnObjectOfAnotherWorldCannotBeTaken)
{
    const std::unique_ptr<Scene> scene = MakeScene("shared", {1, 2});
    NetworkWorld first;
    NetworkWorld second;
    ASSERT_TRUE(first.BindScene(*scene).has_value());
    const auto taken = second.BindScene(*scene);
    ASSERT_FALSE(taken.has_value());
    EXPECT_NE(taken.error().find("another network world"), std::string::npos);
    // The first world is untouched
    EXPECT_EQ(first.Count(), 2u);
    EXPECT_EQ(IdOf(*scene, 1), 1u);
}

TEST(NetworkWorldTest, BindingAgainStartsOver)
{
    const std::unique_ptr<Scene> scene = MakeScene("rebind", {4, 8});
    const ScopedPrefab prefab("WorldTest_Rebind");
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    const auto spawned = world.SpawnInstance(world.AllocateId(), prefab.Name(), 0);
    ASSERT_TRUE(spawned.has_value());
    EXPECT_EQ(world.Count(), 3u);

    const auto again = world.BindScene(*scene);
    ASSERT_TRUE(again.has_value());
    // The spawned object is still in the scene, but without an id it is a plain scene object of the second bind only if
    // it has a NetworkObject: it does, so there are three scene objects now
    EXPECT_EQ(*again, 3u);
    EXPECT_EQ(world.Count(), 3u);
    EXPECT_FALSE((*spawned)->IsSpawned());
}

TEST(NetworkWorldTest, IdsOnlyRiseAndAreNeverReused)
{
    const std::unique_ptr<Scene> scene = MakeScene("ids", {1, 2});
    const ScopedPrefab prefab("WorldTest_Ids");
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());

    const NetworkId a = world.AllocateId();
    EXPECT_EQ(a.value, 3u);
    ASSERT_TRUE(world.SpawnInstance(a, prefab.Name(), 0).has_value());
    const NetworkId b = world.AllocateId();
    EXPECT_EQ(b.value, 4u);
    ASSERT_TRUE(world.SpawnInstance(b, prefab.Name(), 0).has_value());

    EXPECT_TRUE(world.Despawn(a).found);
    EXPECT_TRUE(world.Despawn(b).found);
    // Despawned ids are gone for good: the next one is new
    EXPECT_EQ(world.AllocateId().value, 5u);
    EXPECT_EQ(world.AllocateId().value, 6u);
    EXPECT_EQ(world.Find(a), nullptr);
}

TEST(NetworkWorldTest, SpawningInstantiatesThePrefabIntoTheScene)
{
    const std::unique_ptr<Scene> scene = MakeScene("spawn", {1});
    const ScopedPrefab prefab("WorldTest_Spawn");
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    const std::size_t rootsBefore = scene->GetRootGameObjectCount();

    const NetworkId id = world.AllocateId();
    const auto spawned = world.SpawnInstance(id, prefab.Name(), 7);
    ASSERT_TRUE(spawned.has_value());
    NetworkObject *object = *spawned;
    EXPECT_EQ(object->GetNetworkId(), id);
    EXPECT_EQ(object->GetOwner(), 7u);
    EXPECT_TRUE(object->IsSpawned());
    EXPECT_EQ(object->GetPrefabName(), prefab.Name());
    EXPECT_TRUE(object->IsRegistered());
    EXPECT_EQ(object->GetGameObject().GetScene(), scene.get());
    EXPECT_EQ(scene->GetRootGameObjectCount(), rootsBefore + 1);
    EXPECT_EQ(world.Find(id), object);
    EXPECT_EQ(world.Count(), 2u);
    // The scene object count is the scene's alone
    EXPECT_EQ(world.SceneObjectCount(), 1u);

    // Another instance is another object with another UUID
    const NetworkId second = world.AllocateId();
    const auto other = world.SpawnInstance(second, prefab.Name(), 0);
    ASSERT_TRUE(other.has_value());
    EXPECT_NE((*other)->GetGameObject().GetUUID(), object->GetGameObject().GetUUID());
}

TEST(NetworkWorldTest, SpawningChecksEverythingBeforeItTouchesTheScene)
{
    const std::unique_ptr<Scene> scene = MakeScene("spawn-checks", {1});
    const ScopedPrefab good("WorldTest_Checks");
    const ScopedPrefab none("WorldTest_ChecksNone", ScopedPrefab::Shape::None);
    const ScopedPrefab onChild("WorldTest_ChecksChild", ScopedPrefab::Shape::OneOnChild);
    const ScopedPrefab two("WorldTest_ChecksTwo", ScopedPrefab::Shape::TwoInTree);
    NetworkWorld world;

    // No scene yet
    EXPECT_FALSE(world.SpawnInstance(NetworkId{5}, good.Name(), 0).has_value());

    ASSERT_TRUE(world.BindScene(*scene).has_value());
    const std::size_t rootsBefore = scene->GetRootGameObjectCount();

    const auto invalid = world.SpawnInstance(NetworkId{0}, good.Name(), 0);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_NE(invalid.error().find("not a valid id"), std::string::npos);

    const auto inUse = world.SpawnInstance(NetworkId{1}, good.Name(), 0);
    ASSERT_FALSE(inUse.has_value());
    EXPECT_NE(inUse.error().find("already in use"), std::string::npos);

    const auto unknown = world.SpawnInstance(NetworkId{5}, "WorldTest_NoSuchPrefab", 0);
    ASSERT_FALSE(unknown.has_value());
    EXPECT_NE(unknown.error().find("could not be instantiated"), std::string::npos);

    for (const ScopedPrefab *bad : {&none, &onChild, &two})
    {
        const auto result = world.SpawnInstance(NetworkId{5}, bad->Name(), 0);
        ASSERT_FALSE(result.has_value()) << bad->Name();
        EXPECT_NE(result.error().find("exactly one NetworkObject, on its root"), std::string::npos) << bad->Name();
    }

    // Nothing was added by any failure
    EXPECT_EQ(scene->GetRootGameObjectCount(), rootsBefore);
    EXPECT_EQ(world.Count(), 1u);
    EXPECT_TRUE(world.SpawnInstance(NetworkId{5}, good.Name(), 0).has_value());
}

TEST(NetworkWorldTest, ADespawnDestroysTheObjectWithoutTellingTheHandler)
{
    const std::unique_ptr<Scene> scene = MakeScene("despawn", {1, 2});
    const ScopedPrefab prefab("WorldTest_Despawn");
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    int handlerCalls = 0;
    world.SetDestroyedHandler([&](NetworkId, bool) { ++handlerCalls; });

    const NetworkId id = world.AllocateId();
    const auto spawned = world.SpawnInstance(id, prefab.Name(), 0);
    ASSERT_TRUE(spawned.has_value());
    const GameObject::Ptr spawnedObject = (*spawned)->GetGameObject().shared_from_this();
    const std::size_t rootsWithIt = scene->GetRootGameObjectCount();

    const NetworkWorld::DespawnResult result = world.Despawn(id);
    EXPECT_TRUE(result.found);
    EXPECT_TRUE(result.wasSpawned);
    EXPECT_EQ(world.Find(id), nullptr);
    EXPECT_EQ(world.Count(), 2u);
    scene->ProcessDestroyed();
    EXPECT_TRUE(spawnedObject->IsDestroyed());
    EXPECT_EQ(scene->GetRootGameObjectCount(), rootsWithIt - 1);
    EXPECT_EQ(handlerCalls, 0);

    // A scene object is despawned the same way, and says it was not spawned
    const NetworkWorld::DespawnResult sceneResult = world.Despawn(NetworkId{1});
    EXPECT_TRUE(sceneResult.found);
    EXPECT_FALSE(sceneResult.wasSpawned);
    // Not there any more, and nothing for an id that was never there
    EXPECT_FALSE(world.Despawn(NetworkId{1}).found);
    EXPECT_FALSE(world.Despawn(NetworkId{99}).found);
    EXPECT_FALSE(world.Despawn(NetworkId{0}).found);
    EXPECT_EQ(handlerCalls, 0);
}

TEST(NetworkWorldTest, TheHandlerHearsOfObjectsTheGameDestroys)
{
    const std::unique_ptr<Scene> scene = MakeScene("destroyed", {1, 2});
    const ScopedPrefab prefab("WorldTest_Destroyed");
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    std::vector<std::pair<std::uint32_t, bool>> heard;
    world.SetDestroyedHandler([&](const NetworkId id, const bool spawned) { heard.emplace_back(id.value, spawned); });

    const auto spawned = world.SpawnInstance(world.AllocateId(), prefab.Name(), 0);
    ASSERT_TRUE(spawned.has_value());
    const std::uint32_t spawnedId = (*spawned)->GetNetworkId().value;

    FindByNumber(*scene, 2)->Destroy();
    (*spawned)->GetGameObject().Destroy();
    EXPECT_TRUE(heard.empty()); // destroyed at the scene's next destroy pass, not when asked
    scene->ProcessDestroyed();

    ASSERT_EQ(heard.size(), 2u);
    std::map<std::uint32_t, bool> byId(heard.begin(), heard.end());
    ASSERT_TRUE(byId.contains(2));
    ASSERT_TRUE(byId.contains(spawnedId));
    EXPECT_FALSE(byId[2]);
    EXPECT_TRUE(byId[spawnedId]);
    EXPECT_EQ(world.Count(), 1u);
    EXPECT_EQ(world.Find(NetworkId{2}), nullptr);
    EXPECT_NE(world.Find(NetworkId{1}), nullptr);
}

TEST(NetworkWorldTest, ResetForgetsTheSceneAndLeavesPlainObjectsBehind)
{
    const std::unique_ptr<Scene> scene = MakeScene("reset", {1, 2});
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    int handlerCalls = 0;
    world.SetDestroyedHandler([&](NetworkId, bool) { ++handlerCalls; });
    world.Reset();
    EXPECT_EQ(world.Count(), 0u);
    EXPECT_FALSE(world.HasScene());
    EXPECT_EQ(world.GetScene(), nullptr);
    EXPECT_EQ(world.SceneObjectCount(), 0u);
    EXPECT_EQ(IdOf(*scene, 1), 0u);
    EXPECT_FALSE(FindByNumber(*scene, 1)->GetComponent<NetworkObject>()->IsRegistered());
    EXPECT_EQ(handlerCalls, 0);
    EXPECT_EQ(world.AllocateId().value, 1u);
}

TEST(NetworkWorldTest, FreeingTheSceneLosesTheWorld)
{
    auto scene = MakeScene("freed", {1, 2, 3});
    NetworkWorld world;
    ASSERT_TRUE(world.BindScene(*scene).has_value());
    ASSERT_TRUE(world.HasScene());
    scene.reset();
    EXPECT_FALSE(world.HasScene());
    EXPECT_EQ(world.GetScene(), nullptr);
    // Its objects were freed with it, and left the world as they went
    EXPECT_EQ(world.Count(), 0u);
    EXPECT_FALSE(world.SpawnInstance(NetworkId{9}, "anything", 0).has_value());
}

TEST(NetworkWorldTest, TheWorldMayBeDestroyedBeforeItsScene)
{
    auto scene = MakeScene("outlived", {1, 2});
    {
        NetworkWorld world;
        ASSERT_TRUE(world.BindScene(*scene).has_value());
    }
    // The objects survive the world, as plain objects: nothing dangles
    EXPECT_FALSE(FindByNumber(*scene, 1)->GetComponent<NetworkObject>()->IsRegistered());
    scene.reset();
}
