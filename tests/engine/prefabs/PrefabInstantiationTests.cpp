#include <gtest/gtest.h>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/prefabs/Prefab.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

using namespace N2Engine;
using json = nlohmann::json;

namespace
{
    class PrefabRefTarget final : public SerializableComponent
    {
    public:
        explicit PrefabRefTarget(GameObject &go) : SerializableComponent(go) {}
        [[nodiscard]] std::string GetTypeName() const override { return "PrefabTest_RefTarget"; }
    };

    class PrefabRefHolder final : public SerializableComponent
    {
    public:
        explicit PrefabRefHolder(GameObject &go) : SerializableComponent(go)
        {
            RegisterGameObjectRef("target", target);
            RegisterComponentRef("component", component);
            RegisterComponentRefVector("components", components);
        }
        [[nodiscard]] std::string GetTypeName() const override { return "PrefabTest_RefHolder"; }

        GameObject *target = nullptr;
        PrefabRefTarget *component = nullptr;
        std::vector<PrefabRefTarget *> components;
    };

    void RegisterPrefabTestComponents()
    {
        ComponentRegistry::Instance().Register(
            "PrefabTest_RefTarget",
            [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<PrefabRefTarget>(go); });
        ComponentRegistry::Instance().Register(
            "PrefabTest_RefHolder",
            [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<PrefabRefHolder>(go); });
    }

    // Root "Turret" holding references to its child "Barrel" and the child's component, plus one
    // reference to an object outside the prefab
    struct TurretSource
    {
        GameObject::Ptr root;
        GameObject::Ptr barrel;
        GameObject::Ptr outsider;
        PrefabRefHolder *holder = nullptr;
        PrefabRefTarget *target = nullptr;
    };

    TurretSource MakeTurret()
    {
        RegisterPrefabTestComponents();
        TurretSource source;
        source.root = GameObject::Create("Turret");
        source.barrel = GameObject::Create("Barrel");
        source.outsider = GameObject::Create("Outsider");
        source.root->AddChild(source.barrel);
        source.holder = source.root->AddComponent<PrefabRefHolder>();
        source.target = source.barrel->AddComponent<PrefabRefTarget>();
        source.holder->target = source.barrel.get();
        source.holder->component = source.target;
        source.holder->components = {source.target, nullptr};
        return source;
    }

    void CollectUUIDs(const GameObject &go, std::vector<Math::UUID> &out)
    {
        out.push_back(go.GetUUID());
        for (const auto &component : go.GetAllComponents())
        {
            out.push_back(component->GetUUID());
        }
        for (const auto &child : go.GetChildren())
        {
            CollectUUIDs(*child, out);
        }
    }

    // The error Prefab::Deserialize returns for this data; nullopt (and a test failure if it threw) otherwise
    std::optional<PrefabParseError> DeserializeError(const json &j)
    {
        try
        {
            const auto result = Prefab::Deserialize(j);
            if (result.has_value())
            {
                return std::nullopt;
            }
            return result.error();
        }
        catch (const std::exception &e)
        {
            ADD_FAILURE() << "Prefab::Deserialize threw: " << e.what();
            return std::nullopt;
        }
    }

    bool SharesAny(const std::vector<Math::UUID> &a, const std::vector<Math::UUID> &b)
    {
        for (const auto &x : a)
        {
            for (const auto &y : b)
            {
                if (x == y)
                {
                    return true;
                }
            }
        }
        return false;
    }
}

// ============================================================================
// InstantiatePrefab
// ============================================================================

TEST(PrefabInstantiateTest, AcceptsPrefabJson)
{
    const TurretSource source = MakeTurret();
    const json prefabJson = Prefab("Turret", source.root).Serialize();

    const auto instance = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetName(), "Turret");
    ASSERT_EQ(instance->GetChildCount(), 1u);
    EXPECT_EQ(instance->GetChild(0)->GetName(), "Barrel");
}

TEST(PrefabInstantiateTest, AcceptsBareGameObjectJson)
{
    const TurretSource source = MakeTurret();

    const auto instance = PrefabManager::InstantiatePrefab(source.root->Serialize());
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetName(), "Turret");
}

TEST(PrefabInstantiateTest, EveryObjectAndComponentGetsAFreshUUID)
{
    const TurretSource source = MakeTurret();
    const json prefabJson = Prefab("Turret", source.root).Serialize();

    const auto first = PrefabManager::InstantiatePrefab(prefabJson);
    const auto second = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    std::vector<Math::UUID> sourceIds, firstIds, secondIds;
    CollectUUIDs(*source.root, sourceIds);
    CollectUUIDs(*first, firstIds);
    CollectUUIDs(*second, secondIds);
    ASSERT_EQ(firstIds.size(), sourceIds.size());
    ASSERT_EQ(secondIds.size(), sourceIds.size());

    EXPECT_FALSE(SharesAny(sourceIds, firstIds)) << "an instance kept a prefab UUID";
    EXPECT_FALSE(SharesAny(firstIds, secondIds)) << "two instances share a UUID";
    for (const auto &id : firstIds)
    {
        EXPECT_NE(id, Math::UUID::ZERO);
    }
}

TEST(PrefabInstantiateTest, InternalReferencesPointIntoTheNewInstance)
{
    const TurretSource source = MakeTurret();
    const json prefabJson = Prefab("Turret", source.root).Serialize();

    const auto instance = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(instance, nullptr);
    auto *holder = instance->GetComponent<PrefabRefHolder>();
    ASSERT_NE(holder, nullptr);
    const auto barrel = instance->GetChild(0);
    auto *target = barrel->GetComponent<PrefabRefTarget>();
    ASSERT_NE(target, nullptr);

    EXPECT_EQ(holder->target, barrel.get());
    EXPECT_EQ(holder->component, target);
    ASSERT_EQ(holder->components.size(), 2u);
    EXPECT_EQ(holder->components[0], target);
    EXPECT_EQ(holder->components[1], nullptr);

    // Serializing the instance writes the new UUIDs of what it references
    const json saved = holder->Serialize();
    EXPECT_EQ(saved.at("target").get<std::string>(), barrel->GetUUID().ToString());
    EXPECT_EQ(saved.at("component").get<std::string>(), target->GetUUID().ToString());
}

TEST(PrefabInstantiateTest, TwoInstancesReferenceTheirOwnObjects)
{
    const TurretSource source = MakeTurret();
    const json prefabJson = Prefab("Turret", source.root).Serialize();

    const auto first = PrefabManager::InstantiatePrefab(prefabJson);
    const auto second = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(first->GetComponent<PrefabRefHolder>()->target, first->GetChild(0).get());
    EXPECT_EQ(second->GetComponent<PrefabRefHolder>()->target, second->GetChild(0).get());
}

TEST(PrefabInstantiateTest, ReferencesOutsideThePrefabAreNull)
{
    TurretSource source = MakeTurret();
    source.holder->target = source.outsider.get(); // not part of the prefab
    const json prefabJson = Prefab("Turret", source.root).Serialize();

    const auto instance = PrefabManager::InstantiatePrefab(prefabJson);
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetComponent<PrefabRefHolder>()->target, nullptr);
}

TEST(PrefabInstantiateTest, InstanceIsInNoScene)
{
    const TurretSource source = MakeTurret();
    const auto instance = PrefabManager::InstantiatePrefab(Prefab("Turret", source.root).Serialize());
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetScene(), nullptr);
}

TEST(PrefabInstantiateTest, MalformedDataReturnsNullWithoutThrowing)
{
    std::shared_ptr<GameObject> result;
    EXPECT_NO_THROW(result = PrefabManager::InstantiatePrefab(json{{"name", "NoUUID"}}));
    EXPECT_EQ(result, nullptr);

    EXPECT_NO_THROW(result = PrefabManager::InstantiatePrefab(json{{"name", "P"}, {"rootObject", nullptr}}));
    EXPECT_EQ(result, nullptr);

    EXPECT_NO_THROW(result = PrefabManager::InstantiatePrefab(json{{"name", "P"}, {"rootObject", json::array()}}));
    EXPECT_EQ(result, nullptr);

    EXPECT_NO_THROW(result = PrefabManager::InstantiatePrefab(json(42)));
    EXPECT_EQ(result, nullptr);

    const json badName = {{"uuid", Math::UUID::Random().ToString()}, {"name", 7}};
    EXPECT_NO_THROW(result = PrefabManager::InstantiatePrefab(badName));
    EXPECT_EQ(result, nullptr);
}

// ============================================================================
// RegisterPrefab
// ============================================================================

TEST(PrefabRegistryTest, RegisteredPrefabInstantiatesByName)
{
    const TurretSource source = MakeTurret();
    ASSERT_TRUE(PrefabManager::RegisterPrefab("PrefabRegistry_Turret", source.root));
    EXPECT_TRUE(PrefabManager::HasPrefab("PrefabRegistry_Turret"));

    const auto instance = PrefabManager::InstantiateRegisteredPrefab("PrefabRegistry_Turret");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetName(), "Turret");
    EXPECT_NE(instance->GetUUID(), source.root->GetUUID());
    EXPECT_EQ(instance->GetComponent<PrefabRefHolder>()->target, instance->GetChild(0).get());

    EXPECT_TRUE(PrefabManager::UnregisterPrefab("PrefabRegistry_Turret"));
}

TEST(PrefabRegistryTest, RegisterStoresASnapshot)
{
    const auto root = GameObject::Create("Original");
    ASSERT_TRUE(PrefabManager::RegisterPrefab("PrefabRegistry_Snapshot", root));
    root->SetName("RenamedAfterRegistering");

    const auto instance = PrefabManager::InstantiateRegisteredPrefab("PrefabRegistry_Snapshot");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetName(), "Original");
    EXPECT_NE(instance, root);

    PrefabManager::UnregisterPrefab("PrefabRegistry_Snapshot");
}

TEST(PrefabRegistryTest, RegisteringAgainReplaces)
{
    ASSERT_TRUE(PrefabManager::RegisterPrefab("PrefabRegistry_Replace", GameObject::Create("First")));
    ASSERT_TRUE(PrefabManager::RegisterPrefab("PrefabRegistry_Replace", GameObject::Create("Second")));

    const auto instance = PrefabManager::InstantiateRegisteredPrefab("PrefabRegistry_Replace");
    ASSERT_NE(instance, nullptr);
    EXPECT_EQ(instance->GetName(), "Second");

    PrefabManager::UnregisterPrefab("PrefabRegistry_Replace");
}

TEST(PrefabRegistryTest, InvalidRegistrationsAreRefused)
{
    EXPECT_FALSE(PrefabManager::RegisterPrefab("", GameObject::Create("NoName")));
    EXPECT_FALSE(PrefabManager::RegisterPrefab("PrefabRegistry_NullRoot", nullptr));
    EXPECT_FALSE(PrefabManager::HasPrefab("PrefabRegistry_NullRoot"));
}

TEST(PrefabRegistryTest, UnknownNameGivesNull)
{
    EXPECT_FALSE(PrefabManager::HasPrefab("PrefabRegistry_Unknown"));
    EXPECT_EQ(PrefabManager::InstantiateRegisteredPrefab("PrefabRegistry_Unknown"), nullptr);
    EXPECT_FALSE(PrefabManager::UnregisterPrefab("PrefabRegistry_Unknown"));
}

// ============================================================================
// Prefab::Deserialize error values
// ============================================================================

TEST(PrefabDeserializeErrorTest, MalformedRootIsAnErrorValueNotAnException)
{
    const json missingUuid = {{"name", "P"}, {"rootObject", {{"name", "Root"}}}};
    EXPECT_TRUE(DeserializeError(missingUuid) == PrefabParseError::InvalidRootObject);

    const json wrongType = {{"name", "P"}, {"rootObject", "not an object"}};
    EXPECT_TRUE(DeserializeError(wrongType) == PrefabParseError::InvalidRootObject);
}

TEST(PrefabDeserializeErrorTest, NonStringNameIsInvalidName)
{
    const json j = {{"name", 12}, {"rootObject", GameObject::Create("Root")->Serialize()}};
    EXPECT_TRUE(DeserializeError(j) == PrefabParseError::InvalidName);
}

TEST(PrefabDeserializeErrorTest, NonObjectDataIsMissingName)
{
    EXPECT_TRUE(DeserializeError(json::array()) == PrefabParseError::MissingName);
}

TEST(PrefabDeserializeErrorTest, ValidPrefabStillKeepsItsUUIDs)
{
    const auto root = GameObject::Create("Kept");
    const auto result = Prefab::Deserialize(Prefab("Kept", root).Serialize());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)->GetRootObject()->GetUUID(), root->GetUUID());
}
