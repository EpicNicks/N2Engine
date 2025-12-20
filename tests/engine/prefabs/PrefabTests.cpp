#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/GameObject.hpp"
#include "engine/prefabs/Prefab.hpp"
#include "engine/Positionable.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

using namespace N2Engine;
using json = nlohmann::json;

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

TEST_F(GameObjectSerializationTest, WithChildren_Deserialize)
{
    json j = {
        {"uuid", "parent-uuid"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {"children", {
            {
                {"uuid", "child1-uuid"},
                {"name", "Child1"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            },
            {
                {"uuid", "child2-uuid"},
                {"name", "Child2"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            }
        }}
    };

    auto go = GameObject::Deserialize(j);

    ASSERT_NE(go, nullptr);
    EXPECT_EQ(go->GetChildCount(), 2);
    EXPECT_EQ(go->GetChild(0)->GetName(), "Child1");
    EXPECT_EQ(go->GetChild(1)->GetName(), "Child2");
}

TEST_F(GameObjectSerializationTest, ChildrenHaveParent_Deserialize)
{
    json j = {
        {"uuid", "parent-uuid"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {"children", {
            {
                {"uuid", "child-uuid"},
                {"name", "Child"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            }
        }}
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
        {"uuid", "root-uuid"},
        {"name", "Root"},
        {"isActive", true},
        {"components", json::array()},
        {"children", {
            {
                {"uuid", "child-uuid"},
                {"name", "Child"},
                {"isActive", true},
                {"components", json::array()},
                {"children", {
                    {
                        {"uuid", "grandchild-uuid"},
                        {"name", "Grandchild"},
                        {"isActive", true},
                        {"components", json::array()},
                        {"children", json::array()}
                    }
                }}
            }
        }}
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
        {"uuid", "test-uuid-1234"},
        {"name", "Test"},
        {"isActive", true},
        {"components", json::array()},
        {"children", json::array()}
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    ASSERT_NE(go, nullptr);
    
    Math::UUID uuid("test-uuid-1234");
    auto found = resolver.FindGameObject(uuid);
    EXPECT_EQ(found, go.get());
}

TEST_F(GameObjectSerializationTest, ReferenceResolver_RegistersChildren)
{
    json j = {
        {"uuid", "parent-uuid"},
        {"name", "Parent"},
        {"isActive", true},
        {"components", json::array()},
        {"children", {
            {
                {"uuid", "child-uuid"},
                {"name", "Child"},
                {"isActive", true},
                {"components", json::array()},
                {"children", json::array()}
            }
        }}
    };

    ReferenceResolver resolver;
    auto go = GameObject::Deserialize(j, &resolver);

    Math::UUID childUUID("child-uuid");
    auto foundChild = resolver.FindGameObject(childUUID);
    EXPECT_NE(foundChild, nullptr);
    EXPECT_EQ(foundChild->GetName(), "Child");
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
        {"rootObject", {
            {"uuid", "root-uuid"},
            {"name", "PrefabRoot"},
            {"isActive", true},
            {"components", json::array()},
            {"children", json::array()}
        }}
    };

    auto result = Prefab::Deserialize(j);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value()->GetName(), "DeserializedPrefab");
    EXPECT_EQ(result.value()->GetRootObject()->GetName(), "PrefabRoot");
}

TEST_F(PrefabSerializationTest, MissingName_Deserialize)
{
    json j = {
        {"rootObject", {
            {"uuid", "root-uuid"},
            {"name", "Root"},
            {"isActive", true},
            {"components", json::array()},
            {"children", json::array()}
        }}
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
        {"rootObject", {
            {"uuid", "root-uuid"},
            {"name", "Root"},
            {"isActive", true},
            {"components", json::array()},
            {"children", {
                {
                    {"uuid", "child-uuid"},
                    {"name", "Child"},
                    {"isActive", true},
                    {"components", json::array()},
                    {"children", json::array()}
                }
            }}
        }}
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

TEST_F(PrefabSerializationTest, ComplexHierarchy_RoundTrip)
{
    auto root = GameObject::Create("Root");
    auto child1 = GameObject::Create("Child1");
    auto child2 = GameObject::Create("Child2");
    auto grandchild = GameObject::Create("Grandchild");
    
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
}

// Prefab Error String Tests
TEST(PrefabParseErrorTest, AllErrorsHaveStrings)
{
    EXPECT_FALSE(PrefabParseErrorToString(PrefabParseError::MissingName).empty());
    EXPECT_FALSE(PrefabParseErrorToString(PrefabParseError::MissingRootObject).empty());
    EXPECT_FALSE(PrefabParseErrorToString(PrefabParseError::InvalidRootObject).empty());
}