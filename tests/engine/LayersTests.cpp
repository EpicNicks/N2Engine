#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"

// The layer registry and the GameObject layer field; no physics backend needed (PhysX builds test the
// filtering in physics/LayerPhysicsTests.cpp)

using namespace N2Engine;
using json = nlohmann::json;

// The registry is engine-wide, so every test starts and ends with the defaults
class LayersRegistryTest : public ::testing::Test
{
protected:
    void SetUp() override { Layers::ResetToDefaults(); }
    void TearDown() override { Layers::ResetToDefaults(); }
};

TEST_F(LayersRegistryTest, BuiltInLayersHaveUnityNamesAndIndices)
{
    EXPECT_EQ(Layers::LayerToName(0), "Default");
    EXPECT_EQ(Layers::LayerToName(1), "TransparentFX");
    EXPECT_EQ(Layers::LayerToName(2), "Ignore Raycast");
    EXPECT_EQ(Layers::LayerToName(3), "");
    EXPECT_EQ(Layers::LayerToName(4), "Water");
    EXPECT_EQ(Layers::LayerToName(5), "UI");
    EXPECT_EQ(Layers::LayerToName(31), "");

    EXPECT_EQ(Layers::NameToLayer("Default"), Layers::Default);
    EXPECT_EQ(Layers::NameToLayer("Ignore Raycast"), Layers::IgnoreRaycast);
    EXPECT_EQ(Layers::NameToLayer("UI"), Layers::UI);
    EXPECT_EQ(Layers::NameToLayer("ui"), -1) << "names are case-sensitive";
    EXPECT_EQ(Layers::NameToLayer("Nope"), -1);
    EXPECT_EQ(Layers::NameToLayer(""), -1) << "an unnamed layer can't be looked up";

    EXPECT_EQ(Layers::LayerToName(-1), "");
    EXPECT_EQ(Layers::LayerToName(32), "");
    EXPECT_TRUE(Layers::IsReserved(Layers::Water));
    EXPECT_FALSE(Layers::IsReserved(3));
}

TEST_F(LayersRegistryTest, OnlyUserLayersCanBeNamedAndNamesAreUnique)
{
    EXPECT_TRUE(Layers::SetName(8, "Player"));
    EXPECT_EQ(Layers::NameToLayer("Player"), 8);
    EXPECT_EQ(Layers::LayerToName(8), "Player");

    EXPECT_FALSE(Layers::SetName(Layers::Default, "Ground")) << "built-in layers keep their names";
    EXPECT_EQ(Layers::LayerToName(Layers::Default), "Default");
    EXPECT_FALSE(Layers::SetName(32, "Out"));
    EXPECT_FALSE(Layers::SetName(-1, "Out"));

    EXPECT_FALSE(Layers::SetName(9, "Player")) << "NameToLayer would be ambiguous";
    EXPECT_FALSE(Layers::SetName(9, "Water"));
    EXPECT_TRUE(Layers::SetName(8, "Player")) << "renaming a layer to its own name is fine";

    EXPECT_TRUE(Layers::SetName(8, ""));
    EXPECT_EQ(Layers::NameToLayer("Player"), -1);
    EXPECT_TRUE(Layers::SetName(9, "Player"));
}

TEST_F(LayersRegistryTest, MasksFromIndicesAndNames)
{
    EXPECT_EQ(Layers::MaskOf(0), 1u);
    EXPECT_EQ(Layers::MaskOf(31), 0x80000000u);
    EXPECT_EQ(Layers::MaskOf(32), 0u);
    EXPECT_EQ(Layers::MaskOf(-1), 0u);
    EXPECT_EQ(Layers::DefaultRaycastMask, 0xFFFFFFFBu) << "every layer but Ignore Raycast";

    ASSERT_TRUE(Layers::SetName(8, "Enemy"));
    EXPECT_EQ(Layers::GetMask("Default", "Water"), (1u << 0) | (1u << 4));
    EXPECT_EQ(Layers::GetMask(std::string("Enemy")), 1u << 8);
    EXPECT_EQ(Layers::GetMask("Enemy", "NoSuchLayer"), 1u << 8) << "unknown names are skipped";
    EXPECT_EQ(Layers::GetMask(), 0u);
    EXPECT_EQ(Layers::GetMask(std::vector<std::string>{"UI", "Enemy"}), (1u << 5) | (1u << 8));
}

TEST_F(LayersRegistryTest, CollisionMatrixIsSymmetricAndCollidesByDefault)
{
    for (int layer = 0; layer < Layers::Count; ++layer)
    {
        EXPECT_EQ(Layers::CollisionMaskFor(layer), Layers::AllLayers);
    }

    Layers::SetCollision(3, 9, false);
    EXPECT_FALSE(Layers::GetCollision(3, 9));
    EXPECT_FALSE(Layers::GetCollision(9, 3));
    EXPECT_EQ(Layers::CollisionMaskFor(3), ~(1u << 9));
    EXPECT_EQ(Layers::CollisionMaskFor(9), ~(1u << 3));
    EXPECT_TRUE(Layers::GetCollision(3, 8));

    Layers::SetCollision(6, 6, false); // a layer with itself
    EXPECT_FALSE(Layers::GetCollision(6, 6));

    Layers::SetCollision(9, 3, true);
    EXPECT_TRUE(Layers::GetCollision(3, 9));

    // Out-of-range layers are ignored
    Layers::SetCollision(-1, 3, false);
    Layers::SetCollision(3, 32, false);
    EXPECT_EQ(Layers::CollisionMaskFor(3), Layers::AllLayers);
    EXPECT_FALSE(Layers::GetCollision(-1, 0));
    EXPECT_EQ(Layers::CollisionMaskFor(32), 0u);
}

TEST_F(LayersRegistryTest, ClampBringsIndicesIntoRange)
{
    EXPECT_EQ(Layers::Clamp(7, "test"), 7);
    EXPECT_EQ(Layers::Clamp(-3, "test"), 0);
    EXPECT_EQ(Layers::Clamp(40, "test"), 31);
}

TEST_F(LayersRegistryTest, JsonRoundTrip)
{
    ASSERT_TRUE(Layers::SetName(8, "Player"));
    ASSERT_TRUE(Layers::SetName(31, "Last"));
    Layers::SetCollision(8, 31, false);
    Layers::SetCollision(0, 8, false);

    const json saved = Layers::Serialize();
    ASSERT_TRUE(saved.contains("layers"));
    ASSERT_TRUE(saved.contains("collisionMatrix"));
    EXPECT_EQ(saved["layers"].size(), 32u);
    EXPECT_EQ(saved["collisionMatrix"].size(), 32u);
    EXPECT_EQ(saved["layers"][2], "Ignore Raycast");

    Layers::ResetToDefaults();
    EXPECT_EQ(Layers::NameToLayer("Player"), -1);

    ASSERT_TRUE(Layers::Deserialize(saved));
    EXPECT_EQ(Layers::NameToLayer("Player"), 8);
    EXPECT_EQ(Layers::NameToLayer("Last"), 31);
    EXPECT_FALSE(Layers::GetCollision(31, 8));
    EXPECT_FALSE(Layers::GetCollision(8, 0));
    EXPECT_TRUE(Layers::GetCollision(8, 9));
    EXPECT_EQ(Layers::Serialize(), saved);
}

TEST_F(LayersRegistryTest, MissingKeysMeanDefaultsAndBuiltInNamesStay)
{
    ASSERT_TRUE(Layers::SetName(8, "Player"));
    ASSERT_TRUE(Layers::Deserialize(json::object()));
    EXPECT_EQ(Layers::NameToLayer("Player"), -1);
    EXPECT_EQ(Layers::CollisionMaskFor(0), Layers::AllLayers);

    // A block that renames a built-in layer: the built-in name wins, the rest loads
    ASSERT_TRUE(Layers::Deserialize(json{{"layers", {"Ground", "", "", "Props"}}}));
    EXPECT_EQ(Layers::LayerToName(0), "Default");
    EXPECT_EQ(Layers::NameToLayer("Props"), 3);
}

TEST_F(LayersRegistryTest, AsymmetricMatrixCollidesOnlyWhereBothRowsAgree)
{
    json matrix = json::array();
    for (int layer = 0; layer < Layers::Count; ++layer)
    {
        matrix.push_back(Layers::AllLayers);
    }
    matrix[3] = static_cast<uint64_t>(Layers::AllLayers & ~(1u << 9)); // only row 3 rules out 9

    ASSERT_TRUE(Layers::Deserialize(json{{"collisionMatrix", matrix}}));
    EXPECT_FALSE(Layers::GetCollision(3, 9));
    EXPECT_FALSE(Layers::GetCollision(9, 3));
    EXPECT_TRUE(Layers::GetCollision(3, 8));
}

TEST_F(LayersRegistryTest, MatrixBuiltFromSignedIntsLoads)
{
    // nlohmann types these as signed integers, unlike non-negative numbers parsed from text
    json matrix = json::array();
    for (int layer = 0; layer < Layers::Count; ++layer)
    {
        matrix.push_back(layer == 3 ? 0x7FFFFDFF : 0x7FFFFFFF); // row 3 leaves out layer 9; 31 is left out by all
    }
    ASSERT_TRUE(matrix[0].is_number_integer() && !matrix[0].is_number_unsigned());

    ASSERT_TRUE(Layers::Deserialize(json{{"collisionMatrix", matrix}}));
    EXPECT_FALSE(Layers::GetCollision(3, 9));
    EXPECT_TRUE(Layers::GetCollision(3, 8));
    EXPECT_FALSE(Layers::GetCollision(0, 31));
    EXPECT_TRUE(Layers::GetCollision(0, 30));

    // Still rejected: a negative row
    matrix[5] = -1;
    EXPECT_FALSE(Layers::Deserialize(json{{"collisionMatrix", matrix}}));
}

TEST_F(LayersRegistryTest, MalformedJsonChangesNothingAndNeverThrows)
{
    ASSERT_TRUE(Layers::SetName(8, "Player"));
    Layers::SetCollision(8, 9, false);
    const json before = Layers::Serialize();

    json fullMatrix = json::array();
    for (int layer = 0; layer < Layers::Count; ++layer)
    {
        fullMatrix.push_back(Layers::AllLayers);
    }
    const auto withRow = [&fullMatrix](const json &row)
    {
        json matrix = fullMatrix;
        matrix[7] = row;
        return json{{"collisionMatrix", matrix}};
    };

    std::vector<json> tooManyNames(33, "");
    const std::vector<json> bad = {
        json::array(),
        json("layers"),
        json(nullptr),
        json{{"layers", "Player"}},
        json{{"layers", tooManyNames}},
        json{{"layers", json::array({"Default", 5})}},
        json{{"layers", {"", "", "", "Same", "", "", "Same"}}},
        json{{"collisionMatrix", json::array({1, 2, 3})}},
        json{{"collisionMatrix", 7}},
        withRow(-1),
        withRow(static_cast<uint64_t>(0x1FFFFFFFFull)),
        withRow("all"),
        withRow(1.5),
    };

    for (const json &block : bad)
    {
        bool loaded = true;
        EXPECT_NO_THROW(loaded = Layers::Deserialize(block)) << block.dump();
        EXPECT_FALSE(loaded) << block.dump();
        EXPECT_EQ(Layers::Serialize(), before) << "a rejected block changed the layers: " << block.dump();
    }
}

// ============================================================================
// GameObject layer
// ============================================================================

class GameObjectLayerTest : public ::testing::Test
{
protected:
    void SetUp() override { Layers::ResetToDefaults(); }
    void TearDown() override { Layers::ResetToDefaults(); }
};

TEST_F(GameObjectLayerTest, StartsOnDefaultAndClampsOutOfRange)
{
    const auto go = GameObject::Create("Layered");
    EXPECT_EQ(go->GetLayer(), Layers::Default);

    go->SetLayer(9);
    EXPECT_EQ(go->GetLayer(), 9);
    go->SetLayer(40);
    EXPECT_EQ(go->GetLayer(), 31);
    go->SetLayer(-2);
    EXPECT_EQ(go->GetLayer(), 0);
}

TEST_F(GameObjectLayerTest, OnlyRecursiveSetReachesChildren)
{
    const auto parent = GameObject::Create("Parent");
    const auto child = GameObject::Create("Child");
    const auto grandchild = GameObject::Create("Grandchild");
    parent->AddChild(child);
    child->AddChild(grandchild);

    parent->SetLayer(8);
    EXPECT_EQ(child->GetLayer(), 0) << "as in Unity, children keep their own layer";

    parent->SetLayerRecursive(Layers::UI);
    EXPECT_EQ(parent->GetLayer(), Layers::UI);
    EXPECT_EQ(child->GetLayer(), Layers::UI);
    EXPECT_EQ(grandchild->GetLayer(), Layers::UI);
}

TEST_F(GameObjectLayerTest, LayerSurvivesSerialization)
{
    const auto parent = GameObject::Create("Parent");
    const auto child = GameObject::Create("Child");
    parent->AddChild(child);
    parent->SetLayer(9);
    child->SetLayer(Layers::Water);

    const json saved = parent->Serialize();
    EXPECT_EQ(saved.at("layer"), 9);

    const auto loaded = GameObject::Deserialize(saved);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->GetLayer(), 9);
    ASSERT_EQ(loaded->GetChildCount(), 1u);
    EXPECT_EQ(loaded->GetChild(0)->GetLayer(), Layers::Water);
}

TEST_F(GameObjectLayerTest, JsonWithoutLayerLoadsOnDefault)
{
    const auto go = GameObject::Create("Old");
    go->SetLayer(9);
    json saved = go->Serialize();

    saved.erase("layer"); // written before layers existed
    EXPECT_EQ(GameObject::Deserialize(saved)->GetLayer(), Layers::Default);

    saved["layer"] = nullptr;
    EXPECT_EQ(GameObject::Deserialize(saved)->GetLayer(), Layers::Default);
}

TEST_F(GameObjectLayerTest, OutOfRangeLayerInJsonIsClampedAndWrongTypeIsMalformed)
{
    json saved = GameObject::Create("Odd")->Serialize();

    saved["layer"] = 99;
    EXPECT_EQ(GameObject::Deserialize(saved)->GetLayer(), 31);
    saved["layer"] = -5;
    EXPECT_EQ(GameObject::Deserialize(saved)->GetLayer(), 0);
    saved["layer"] = static_cast<uint64_t>(1) << 40;
    EXPECT_EQ(GameObject::Deserialize(saved)->GetLayer(), 31);

    // Like a non-string tag: the object (and the scene loading it) fails to load
    saved["layer"] = "Water";
    EXPECT_ANY_THROW(GameObject::Deserialize(saved));
    saved["layer"] = 2.5;
    EXPECT_ANY_THROW(GameObject::Deserialize(saved));
    saved["layer"] = true;
    EXPECT_ANY_THROW(GameObject::Deserialize(saved));
}
