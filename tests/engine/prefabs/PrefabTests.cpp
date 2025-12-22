#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/GameObject.hpp"
#include "engine/prefabs/Prefab.hpp"
#include "engine/Positionable.hpp"
#include "engine/serialization/ReferenceResolver.hpp"
#include "engine/serialization/MathSerialization.hpp"
#include "math/UUID.hpp"

using namespace N2Engine;
using json = nlohmann::json;

// UUID Serialization/Deserialization Tests

class UUIDSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(UUIDSerializationTest, UUIDSerializesToValidString)
{
    Math::UUID uuid = Math::UUID::Random();
    json j = uuid;

    EXPECT_TRUE(j.is_string());
    std::string uuidStr = j.get<std::string>();

    // Check format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    EXPECT_EQ(uuidStr.length(), 36);
    EXPECT_EQ(uuidStr[8], '-');
    EXPECT_EQ(uuidStr[13], '-');
    EXPECT_EQ(uuidStr[18], '-');
    EXPECT_EQ(uuidStr[23], '-');
}

TEST_F(UUIDSerializationTest, UUIDRoundTripPreservesValue)
{
    Math::UUID original = Math::UUID::Random();
    json j = original;
    Math::UUID deserialized = j.get<Math::UUID>();

    EXPECT_EQ(original, deserialized);
    EXPECT_EQ(original.ToString(), deserialized.ToString());
}

TEST_F(UUIDSerializationTest, MultipleUUIDsSerializeToDifferentValues)
{
    Math::UUID uuid1 = Math::UUID::Random();
    Math::UUID uuid2 = Math::UUID::Random();

    nlohmann::json j1 = uuid1;
    nlohmann::json j2 = uuid2;

    EXPECT_NE(j1.get<std::string>(), j2.get<std::string>());
    EXPECT_NE(uuid1, uuid2);
}

TEST_F(UUIDSerializationTest, ValidUUIDStringDeserializesCorrectly)
{
    std::string uuidStr = "550e8400-e29b-41d4-a716-446655440000";
    nlohmann::json j = uuidStr;

    Math::UUID uuid = j.get<Math::UUID>();
    nlohmann::json serialized = uuid;

    EXPECT_EQ(serialized.get<std::string>(), uuidStr);
}

TEST_F(UUIDSerializationTest, InvalidUUIDFormatReturnsNullopt)
{
    std::vector<std::string> invalidFormats = {
        "not-a-uuid",
        "12345",
        "550e8400-e29b-41d4-a716", // Too short
        "550e8400-e29b-41d4-a716-446655440000-extra", // Too long
        "550e8400-e29b-41d4-a716-44665544000g", // Invalid character
        "",
        "550e840-0e29b-41d4-a716-446655440000", // Wrong dash position
        "child-uuid-5678" // Not a valid UUID
    };

    for (const auto &invalidStr : invalidFormats)
    {
        auto result = Math::UUID::FromString(invalidStr);
        EXPECT_FALSE(result.has_value()) << "Should reject: " << invalidStr;
    }
}

TEST_F(UUIDSerializationTest, ZeroUUIDSerializesAndDeserializes)
{
    Math::UUID zero = Math::UUID::ZERO;
    json j = zero;

    EXPECT_EQ(j.get<std::string>(), "00000000-0000-0000-0000-000000000000");

    Math::UUID deserialized = j.get<Math::UUID>();
    EXPECT_EQ(deserialized, Math::UUID::ZERO);
    EXPECT_TRUE(deserialized == Math::UUID::ZERO);
}

TEST_F(UUIDSerializationTest, FromStringCreatesCorrectUUID)
{
    std::string uuidStr = "550e8400-e29b-41d4-a716-446655440000";
    auto result = Math::UUID::FromString(uuidStr);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->ToString(), uuidStr);
}

TEST_F(UUIDSerializationTest, UUIDArraySerializesCorrectly)
{
    Math::UUID uuid1 = Math::UUID::Random();
    Math::UUID uuid2 = Math::UUID::Random();
    Math::UUID uuid3 = Math::UUID::Random();

    json j = json::array({uuid1, uuid2, uuid3});

    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 3);

    for (size_t i = 0; i < 3; ++i)
    {
        EXPECT_TRUE(j[i].is_string());
    }

    // Verify each can be deserialized
    Math::UUID d1 = j[0].get<Math::UUID>();
    Math::UUID d2 = j[1].get<Math::UUID>();
    Math::UUID d3 = j[2].get<Math::UUID>();

    EXPECT_EQ(d1, uuid1);
    EXPECT_EQ(d2, uuid2);
    EXPECT_EQ(d3, uuid3);
}

TEST_F(UUIDSerializationTest, UUIDInObjectSerializesCorrectly)
{
    Math::UUID uuid = Math::UUID::Random();

    json j = {
        {"id", uuid},
        {"name", "test"},
        {"count", 42}
    };

    EXPECT_TRUE(j["id"].is_string());
    Math::UUID deserialized = j["id"].get<Math::UUID>();
    EXPECT_EQ(deserialized, uuid);
}

// GameObject Serialization Tests

class GameObjectSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(GameObjectSerializationTest, BasicProperties_Serialize)
{
    auto go = GameObject::Create("TestObject");

    json j = go->Serialize();

    EXPECT_TRUE(j.contains("uuid"));
    EXPECT_TRUE(j.contains("name"));
    EXPECT_TRUE(j.contains("isActive"));
    EXPECT_EQ(j["name"], "TestObject");
    EXPECT_EQ(j["isActive"], true);
}

TEST_F(GameObjectSerializationTest, InactiveGameObject_Serialize)
{
    auto go = GameObject::Create("Inactive");
    go->SetActive(false);

    json j = go->Serialize();

    EXPECT_EQ(j["isActive"], false);
}

TEST_F(GameObjectSerializationTest, WithPositionable_Serialize)
{
    auto go = GameObject::Create("Positioned");
    go->CreatePositionable();

    json j = go->Serialize();

    EXPECT_TRUE(j.contains("positionable"));
    EXPECT_TRUE(j["positionable"].is_object());
}

TEST_F(GameObjectSerializationTest, WithoutPositionable_Serialize)
{
    auto go = GameObject::Create("NoPosition");

    json j = go->Serialize();

    EXPECT_FALSE(j.contains("positionable"));
}

TEST_F(GameObjectSerializationTest, EmptyComponents_Serialize)
{
    auto go = GameObject::Create("NoComponents");

    json j = go->Serialize();

    EXPECT_TRUE(j.contains("components"));
    EXPECT_TRUE(j["components"].is_array());
    EXPECT_TRUE(j["components"].empty());
}

TEST_F(GameObjectSerializationTest, EmptyChildren_Serialize)
{
    auto go = GameObject::Create("NoChildren");

    json j = go->Serialize();

    EXPECT_TRUE(j.contains("children"));
    EXPECT_TRUE(j["children"].is_array());
    EXPECT_TRUE(j["children"].empty());
}

TEST_F(GameObjectSerializationTest, WithChildren_Serialize)
{
    auto parent = GameObject::Create("Parent");
    auto child1 = GameObject::Create("Child1");
    auto child2 = GameObject::Create("Child2");

    parent->AddChild(child1, false);
    parent->AddChild(child2, false);

    json j = parent->Serialize();

    EXPECT_EQ(j["children"].size(), 2);
    EXPECT_EQ(j["children"][0]["name"], "Child1");
    EXPECT_EQ(j["children"][1]["name"], "Child2");
}

TEST_F(GameObjectSerializationTest, NestedChildren_Serialize)
{
    auto root = GameObject::Create("Root");
    auto child = GameObject::Create("Child");
    auto grandchild = GameObject::Create("Grandchild");

    root->AddChild(child, false);
    child->AddChild(grandchild, false);

    json j = root->Serialize();

    EXPECT_EQ(j["children"].size(), 1);
    EXPECT_EQ(j["children"][0]["name"], "Child");
    EXPECT_EQ(j["children"][0]["children"].size(), 1);
    EXPECT_EQ(j["children"][0]["children"][0]["name"], "Grandchild");
}

TEST_F(GameObjectSerializationTest, UUIDPreserved_Serialize)
{
    auto go = GameObject::Create("Test");
    auto originalUUID = go->GetUUID().ToString();

    json j = go->Serialize();

    EXPECT_EQ(j["uuid"], originalUUID);
}

TEST_F(GameObjectSerializationTest, UUIDIsValidFormat_Serialize)
{
    auto go = GameObject::Create("Test");
    json j = go->Serialize();

    EXPECT_TRUE(j["uuid"].is_string());
    std::string uuidStr = j["uuid"].get<std::string>();

    // Verify UUID format
    EXPECT_EQ(uuidStr.length(), 36);
    EXPECT_EQ(uuidStr[8], '-');
    EXPECT_EQ(uuidStr[13], '-');
    EXPECT_EQ(uuidStr[18], '-');
    EXPECT_EQ(uuidStr[23], '-');
}

// GameObject Deserialization Tests

TEST_F(GameObjectSerializationTest, BasicProperties_Deserialize)
{
    json j = {
        {"uuid", "12345678-1234-1234-1234-123456789abc"},
        {"name", "DeserializedObject"},
        {"isActive", true},
        {"components", json::array()},
        {"children", json::array()}
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->GetName(), "DeserializedObject");
    EXPECT_EQ(go->IsActive(), true);
}

TEST_F(GameObjectSerializationTest, InactiveGameObject_Deserialize)
{
    json j = {
        {"uuid", "12345678-1234-1234-1234-123456789abc"},
        {"name", "Inactive"},
        {"isActive", false},
        {"components", json::array()},
        {"children", json::array()}
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->IsActive(), false);
}

TEST_F(GameObjectSerializationTest, UUIDRestored_Deserialize)
{
    const std::string expectedUUID = "12345678-1234-1234-1234-123456789abc";
    json j = {
        {"uuid", expectedUUID},
        {"name", "Test"},
        {"isActive", true},
        {"components", json::array()},
        {"children", json::array()}
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->GetUUID().ToString(), expectedUUID);
}

TEST_F(GameObjectSerializationTest, InvalidUUID_DeserializeUsesZero)
{
    json j = {
        {"uuid", "not-a-valid-uuid"},
        {"name", "Test"},
        {"isActive", true},
        {"components", json::array()},
        {"children", json::array()}
    };

    auto go = GameObject::Deserialize(j);

    // Should create GameObject with ZERO UUID instead of throwing
    ASSERT_NE(go, nullptr);
    EXPECT_TRUE(go->GetUUID()==Math::UUID::ZERO);
}

TEST_F(GameObjectSerializationTest, WithChildren_Deserialize)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child1"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                },
                {
                    {"uuid", "00000000-0000-0000-0000-000000000003"},
                    {"name", "Child2"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                }
            }
        }
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->GetChildCount(), 2);
    EXPECT_EQ(go->GetChild(0)->GetName(), "Child1");
    EXPECT_EQ(go->GetChild(1)->GetName(), "Child2");
}

TEST_F(GameObjectSerializationTest, ChildrenHaveCorrectUUIDs_Deserialize)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                }
            }
        }
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    ASSERT_EQ(go->GetChildCount(), 1);

    auto child = go->GetChild(0);
    EXPECT_EQ(child->GetUUID().ToString(), "00000000-0000-0000-0000-000000000002");
}

TEST_F(GameObjectSerializationTest, ChildrenHaveParent_Deserialize)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                }
            }
        }
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    ASSERT_EQ(go->GetChildCount(), 1);

    auto child = go->GetChild(0);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->GetParent(), go);
}

TEST_F(GameObjectSerializationTest, NestedChildren_Deserialize)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Root"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child"},
                    {"isActive", true},
                    {"components", json::array()},
                    {
                        "children", {
                            {
                                {"uuid", "00000000-0000-0000-0000-000000000003"},
                                {"name", "Grandchild"},
                                {"isActive", true},
                                {"components", json::array()},
                                {"children", json::array()}
                            }
                        }
                    }
                }
            }
        }
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);

    auto child = go->GetChild(0);
    ASSERT_NE(child, nullptr);

    auto grandchild = child->GetChild(0);
    ASSERT_NE(grandchild, nullptr);
    EXPECT_EQ(grandchild->GetName(), "Grandchild");
}

// GameObject Round Trip Tests

TEST_F(GameObjectSerializationTest, BasicProperties_RoundTrip)
{
    auto original = GameObject::Create("RoundTripTest");
    original->SetActive(false);

    json j = original->Serialize();
    auto deserialized = GameObject::Deserialize(j);

    ASSERT_NE(deserialized, nullptr);
    EXPECT_EQ(deserialized->GetName(), original->GetName());
    EXPECT_EQ(deserialized->IsActive(), original->IsActive());
    EXPECT_EQ(deserialized->GetUUID().ToString(), original->GetUUID().ToString());
}

TEST_F(GameObjectSerializationTest, UUIDPreserved_RoundTrip)
{
    auto original = GameObject::Create("UUIDTest");
    Math::UUID originalUUID = original->GetUUID();

    json j = original->Serialize();
    auto deserialized = GameObject::Deserialize(j);

    ASSERT_NE(deserialized, nullptr);
    EXPECT_EQ(deserialized->GetUUID(), originalUUID);
}

TEST_F(GameObjectSerializationTest, WithChildren_RoundTrip)
{
    auto parent = GameObject::Create("Parent");
    auto child1 = GameObject::Create("Child1");
    auto child2 = GameObject::Create("Child2");
    parent->AddChild(child1, false);
    parent->AddChild(child2, false);

    json j = parent->Serialize();
    auto deserialized = GameObject::Deserialize(j);

    ASSERT_NE(deserialized, nullptr);
    EXPECT_EQ(deserialized->GetChildCount(), 2);
    EXPECT_EQ(deserialized->GetChild(0)->GetName(), "Child1");
    EXPECT_EQ(deserialized->GetChild(1)->GetName(), "Child2");
}

TEST_F(GameObjectSerializationTest, ChildUUIDsPreserved_RoundTrip)
{
    auto parent = GameObject::Create("Parent");
    auto child1 = GameObject::Create("Child1");
    auto child2 = GameObject::Create("Child2");

    Math::UUID child1UUID = child1->GetUUID();
    Math::UUID child2UUID = child2->GetUUID();

    parent->AddChild(child1, false);
    parent->AddChild(child2, false);

    json j = parent->Serialize();
    auto deserialized = GameObject::Deserialize(j);

    ASSERT_NE(deserialized, nullptr);
    ASSERT_EQ(deserialized->GetChildCount(), 2);
    EXPECT_EQ(deserialized->GetChild(0)->GetUUID(), child1UUID);
    EXPECT_EQ(deserialized->GetChild(1)->GetUUID(), child2UUID);
}

TEST_F(GameObjectSerializationTest, DeepHierarchy_RoundTrip)
{
    auto root = GameObject::Create("Root");
    auto current = root;

    for (int i = 0; i < 5; ++i)
    {
        auto child = GameObject::Create("Level" + std::to_string(i));
        current->AddChild(child, false);
        current = child;
    }

    json j = root->Serialize();
    auto deserialized = GameObject::Deserialize(j);

    ASSERT_NE(deserialized, nullptr);

    auto node = deserialized;
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_EQ(node->GetChildCount(), 1);
        node = node->GetChild(0);
        EXPECT_EQ(node->GetName(), "Level" + std::to_string(i));
    }
}

// GameObject with ReferenceResolver Tests

TEST_F(GameObjectSerializationTest, ReferenceResolver_RegistersGameObject)
{
    json j = {
        {"uuid", "12345678-1234-1234-1234-123456789abc"},
        {"name", "Test"},
        {"isActive", true},
        {"components", json::array()},
        {"children", json::array()}
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    ASSERT_NE(go, nullptr);

    auto uuid = Math::UUID::FromString("12345678-1234-1234-1234-123456789abc");
    ASSERT_TRUE(uuid.has_value());

    auto found = resolver.FindGameObject(*uuid);
    EXPECT_EQ(found, go.get());
}

TEST_F(GameObjectSerializationTest, ReferenceResolver_RegistersChildren)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                }
            }
        }
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    auto childUUID = Math::UUID::FromString("00000000-0000-0000-0000-000000000002");
    ASSERT_TRUE(childUUID.has_value());

    auto foundChild = resolver.FindGameObject(*childUUID);
    EXPECT_NE(foundChild, nullptr);
    EXPECT_EQ(foundChild->GetName(), "Child");
}

TEST_F(GameObjectSerializationTest, ReferenceResolver_RegistersNestedChildren)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Root"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child"},
                    {"isActive", true},
                    {"components", json::array()},
                    {
                        "children", {
                            {
                                {"uuid", "00000000-0000-0000-0000-000000000003"},
                                {"name", "Grandchild"},
                                {"isActive", true},
                                {"components", json::array()},
                                {"children", json::array()}
                            }
                        }
                    }
                }
            }
        }
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    auto grandchildUUID = Math::UUID::FromString("00000000-0000-0000-0000-000000000003");
    ASSERT_TRUE(grandchildUUID.has_value());

    auto foundGrandchild = resolver.FindGameObject(*grandchildUUID);
    EXPECT_NE(foundGrandchild, nullptr);
    EXPECT_EQ(foundGrandchild->GetName(), "Grandchild");
}

TEST_F(GameObjectSerializationTest, ReferenceResolver_FindsMultipleGameObjects)
{
    json j = {
        {"uuid", "00000000-0000-0000-0000-000000000001"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {
            "children", {
                {
                    {"uuid", "00000000-0000-0000-0000-000000000002"},
                    {"name", "Child1"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                },
                {
                    {"uuid", "00000000-0000-0000-0000-000000000003"},
                    {"name", "Child2"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                }
            }
        }
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    auto parentUUID = Math::UUID::FromString("00000000-0000-0000-0000-000000000001");
    auto child1UUID = Math::UUID::FromString("00000000-0000-0000-0000-000000000002");
    auto child2UUID = Math::UUID::FromString("00000000-0000-0000-0000-000000000003");

    ASSERT_TRUE(parentUUID.has_value());
    ASSERT_TRUE(child1UUID.has_value());
    ASSERT_TRUE(child2UUID.has_value());

    EXPECT_NE(resolver.FindGameObject(*parentUUID), nullptr);
    EXPECT_NE(resolver.FindGameObject(*child1UUID), nullptr);
    EXPECT_NE(resolver.FindGameObject(*child2UUID), nullptr);

    EXPECT_EQ(resolver.FindGameObject(*parentUUID)->GetName(), "Parent");
    EXPECT_EQ(resolver.FindGameObject(*child1UUID)->GetName(), "Child1");
    EXPECT_EQ(resolver.FindGameObject(*child2UUID)->GetName(), "Child2");
}

TEST_F(GameObjectSerializationTest, ReferenceResolver_NonExistentUUIDReturnsNull)
{
    json j = {
        {"uuid", "12345678-1234-1234-1234-123456789abc"},
        {"name", "Test"},
        {"isActive", true},
        {"components", json::array()},
        {"children", json::array()}
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    auto nonExistentUUID = Math::UUID::FromString("00000000-0000-0000-0000-999999999999");
    ASSERT_TRUE(nonExistentUUID.has_value());

    auto found = resolver.FindGameObject(*nonExistentUUID);
    EXPECT_EQ(found, nullptr);
}

// Prefab Serialization Tests

class PrefabSerializationTest : public ::testing::Test
{
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(PrefabSerializationTest, BasicPrefab_Serialize)
{
    auto rootObject = GameObject::Create("PrefabRoot");
    Prefab prefab("TestPrefab", rootObject);

    json j = prefab.Serialize();

    EXPECT_TRUE(j.contains("name"));
    EXPECT_TRUE(j.contains("rootObject"));
    EXPECT_EQ(j["name"], "TestPrefab");
    EXPECT_TRUE(j["rootObject"].is_object());
}

TEST_F(PrefabSerializationTest, PrefabWithHierarchy_Serialize)
{
    auto root = GameObject::Create("Root");
    auto child = GameObject::Create("Child");
    root->AddChild(child, false);

    Prefab prefab("HierarchyPrefab", root);

    json j = prefab.Serialize();

    EXPECT_EQ(j["rootObject"]["name"], "Root");
    EXPECT_EQ(j["rootObject"]["children"].size(), 1);
    EXPECT_EQ(j["rootObject"]["children"][0]["name"], "Child");
}

TEST_F(PrefabSerializationTest, PrefabPreservesUUIDs_Serialize)
{
    auto root = GameObject::Create("Root");
    Math::UUID rootUUID = root->GetUUID();

    Prefab prefab("UUIDPrefab", root);
    json j = prefab.Serialize();

    EXPECT_EQ(j["rootObject"]["uuid"].get<std::string>(), rootUUID.ToString());
}

TEST_F(PrefabSerializationTest, GettersWork)
{
    auto rootObject = GameObject::Create("Root");
    Prefab prefab("MyPrefab", rootObject);

    EXPECT_EQ(prefab.GetName(), "MyPrefab");
    EXPECT_EQ(prefab.GetRootObject(), rootObject);
}

// Prefab Deserialization Tests

TEST_F(PrefabSerializationTest, BasicPrefab_Deserialize)
{
    json j = {
        {"name", "DeserializedPrefab"},
        {
            "rootObject", {
                {"uuid", "12345678-1234-1234-1234-123456789abc"},
                {"name", "PrefabRoot"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            }
        }
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetName(), "DeserializedPrefab");
    EXPECT_EQ(result.value()->GetRootObject()->GetName(), "PrefabRoot");
}

TEST_F(PrefabSerializationTest, PrefabPreservesUUIDs_Deserialize)
{
    const std::string expectedUUID = "550e8400-e29b-41d4-a716-446655440000";

    json j = {
        {"name", "UUIDPrefab"},
        {
            "rootObject", {
                {"uuid", expectedUUID},
                {"name", "Root"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            }
        }
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetRootObject()->GetUUID().ToString(), expectedUUID);
}

TEST_F(PrefabSerializationTest, MissingName_Deserialize)
{
    json j = {
        {
            "rootObject", {
                {"uuid", "12345678-1234-1234-1234-123456789abc"},
                {"name", "Root"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            }
        }
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PrefabParseError::MissingName);
}

TEST_F(PrefabSerializationTest, MissingRootObject_Deserialize)
{
    json j = {
        {"name", "NoPrefab"}
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PrefabParseError::MissingRootObject);
}

TEST_F(PrefabSerializationTest, NullRootObject_Deserialize)
{
    json j = {
        {"name", "NullPrefab"},
        {"rootObject", nullptr}
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PrefabParseError::MissingRootObject);
}

TEST_F(PrefabSerializationTest, WithHierarchy_Deserialize)
{
    json j = {
        {"name", "HierarchyPrefab"},
        {
            "rootObject", {
                {"uuid", "00000000-0000-0000-0000-000000000001"},
                {"name", "Root"},
                {"isActive", true},
                {"components", json::array()},
                {
                    "children", {
                        {
                            {"uuid", "00000000-0000-0000-0000-000000000002"},
                            {"name", "Child"},
                            {"isActive", true},
                            {"components", json::array()},
                            {"children", json::array()}
                        }
                    }
                }
            }
        }
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());
    auto root = result.value()->GetRootObject();
    EXPECT_EQ(root->GetChildCount(), 1);
    EXPECT_EQ(root->GetChild(0)->GetName(), "Child");
}

// Prefab Round Trip Tests

TEST_F(PrefabSerializationTest, BasicPrefab_RoundTrip)
{
    auto rootObject = GameObject::Create("OriginalRoot");
    Prefab original("RoundTripPrefab", rootObject);

    json j = original.Serialize();
    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetName(), original.GetName());
    EXPECT_EQ(result.value()->GetRootObject()->GetName(), original.GetRootObject()->GetName());
}

TEST_F(PrefabSerializationTest, UUIDsPreserved_RoundTrip)
{
    auto rootObject = GameObject::Create("Root");
    Math::UUID originalUUID = rootObject->GetUUID();

    Prefab original("UUIDPrefab", rootObject);

    json j = original.Serialize();
    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetRootObject()->GetUUID(), originalUUID);
}

TEST_F(PrefabSerializationTest, ComplexHierarchy_RoundTrip)
{
    auto root = GameObject::Create("Root");
    auto child1 = GameObject::Create("Child1");
    auto child2 = GameObject::Create("Child2");
    auto grandchild = GameObject::Create("Grandchild");

    Math::UUID rootUUID = root->GetUUID();
    Math::UUID child1UUID = child1->GetUUID();
    Math::UUID child2UUID = child2->GetUUID();
    Math::UUID grandchildUUID = grandchild->GetUUID();

    root->AddChild(child1, false);
    root->AddChild(child2, false);
    child1->AddChild(grandchild, false);

    Prefab original("ComplexPrefab", root);

    json j = original.Serialize();
    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());

    auto deserializedRoot = result.value()->GetRootObject();
    EXPECT_EQ(deserializedRoot->GetChildCount(), 2);
    EXPECT_EQ(deserializedRoot->GetChild(0)->GetChildCount(), 1);
    EXPECT_EQ(deserializedRoot->GetChild(0)->GetChild(0)->GetName(), "Grandchild");

    // Verify UUIDs are preserved
    EXPECT_EQ(deserializedRoot->GetUUID(), rootUUID);
    EXPECT_EQ(deserializedRoot->GetChild(0)->GetUUID(), child1UUID);
    EXPECT_EQ(deserializedRoot->GetChild(1)->GetUUID(), child2UUID);
    EXPECT_EQ(deserializedRoot->GetChild(0)->GetChild(0)->GetUUID(), grandchildUUID);
}

// Prefab Error String Tests

TEST(PrefabParseErrorTest, AllErrorsHaveStrings)
{
    EXPECT_FALSE(PrefabParseErrorToString(PrefabParseError::MissingName).empty());
    EXPECT_FALSE(PrefabParseErrorToString(PrefabParseError::MissingRootObject).empty());
    EXPECT_FALSE(PrefabParseErrorToString(PrefabParseError::InvalidRootObject).empty());
}
