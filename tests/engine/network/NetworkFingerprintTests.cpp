#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "NetworkTestSupport.hpp"
#include "engine/Version.hpp"
#include "engine/network/NetworkFingerprint.hpp"
#include "engine/network/PrefabTable.hpp"
#include "engine/prefabs/PrefabManager.hpp"

// The join fingerprint (netcode stage B1): what it covers, what it ignores, which difference it names first, and the
// frozen prefab table whose hash is part of it.

using namespace N2Engine;
using namespace N2Engine::Network;
using namespace N2Engine::Network::Testing;

namespace
{
    Fingerprint SameFingerprint()
    {
        Fingerprint fingerprint;
        fingerprint.engineVersion = "1.0.0";
        fingerprint.sceneHash = 11;
        fingerprint.registryHash = 22;
        fingerprint.scenePath = "res://scenes/a.scene";
        fingerprint.sceneObjectCount = 5;
        fingerprint.prefabCount = 2;
        return fingerprint;
    }
}

TEST(NetworkFingerprintTest, SceneHashIgnoresWhitespaceAndKeyOrderButNotContent)
{
    const auto first = nlohmann::json::parse(R"({"name":"main","rootGameObjects":[{"uuid":"a","x":1.5},{"uuid":"b"}]})");
    const auto reordered =
        nlohmann::json::parse("{\n  \"rootGameObjects\" : [ {\"x\":1.5,\"uuid\":\"a\"}, {\"uuid\":\"b\"} ],\r\n"
                              "  \"name\" : \"main\"\n}");
    const auto changedNumber =
        nlohmann::json::parse(R"({"name":"main","rootGameObjects":[{"uuid":"a","x":1.25},{"uuid":"b"}]})");
    const auto swappedObjects =
        nlohmann::json::parse(R"({"name":"main","rootGameObjects":[{"uuid":"b"},{"uuid":"a","x":1.5}]})");

    EXPECT_EQ(HashSceneJson(first), HashSceneJson(reordered));
    EXPECT_NE(HashSceneJson(first), HashSceneJson(changedNumber));
    // The order of an array is content
    EXPECT_NE(HashSceneJson(first), HashSceneJson(swappedObjects));
}

TEST(NetworkFingerprintTest, SceneHashHandlesDeepNestingAndInvalidUtf8)
{
    // Deep arrays: hashed without recursion
    nlohmann::json deep = nlohmann::json::array();
    nlohmann::json *cursor = &deep;
    for (int level = 0; level < 600; ++level)
    {
        cursor->push_back(nlohmann::json::array());
        cursor = &cursor->back();
    }
    EXPECT_NE(HashSceneJson(deep), 0u);
    EXPECT_EQ(HashSceneJson(deep), HashSceneJson(deep));

    // A string that is not UTF-8 (built, not parsed) is replaced, never an exception
    nlohmann::json bad;
    bad["name"] = std::string("a\xC3");
    EXPECT_NO_THROW({ (void)HashSceneJson(bad); });
}

TEST(NetworkFingerprintTest, SanitizeTextCutsAndReplaces)
{
    EXPECT_EQ(SanitizeText("abc", 10), "abc");
    EXPECT_EQ(SanitizeText("abcdef", 3), "abc");
    EXPECT_EQ(SanitizeText(std::string("a\0b", 3), 10), "a?b");
    EXPECT_EQ(SanitizeText("a\nb\r\tc\x7F", 20), "a?b??c?");
    EXPECT_EQ(SanitizeText("caf\xC3\xA9", 20), "caf??");
    EXPECT_EQ(SanitizeText("", 5), "");
}

TEST(NetworkFingerprintTest, MakeFingerprintDescribesThisProcess)
{
    PrefabTable table;
    const Fingerprint fingerprint = MakeFingerprint("res://scenes/main.scene", 0xABCDull, 9, table);
    EXPECT_EQ(fingerprint.engineVersion, SanitizeText(EngineVersion(), kMaxEngineVersionLength));
    EXPECT_EQ(fingerprint.sceneHash, 0xABCDull);
    EXPECT_EQ(fingerprint.sceneObjectCount, 9u);
    EXPECT_EQ(fingerprint.prefabCount, 0u);
    EXPECT_EQ(fingerprint.scenePath, "res://scenes/main.scene");
    EXPECT_EQ(fingerprint.registryHash, HashRegistry(table));

    // The path is cut and made printable
    const Fingerprint longPath = MakeFingerprint(std::string(500, 'p') + "\n", 1, 1, table);
    EXPECT_EQ(longPath.scenePath.size(), kMaxScenePathLength);
    EXPECT_EQ(longPath.scenePath.find('\n'), std::string::npos);
}

TEST(NetworkFingerprintTest, TheRegistryHashFollowsThePrefabTable)
{
    const PrefabTable empty;
    const PrefabTable one(std::vector<PrefabTable::Entry>{{"Crate", 5}});
    const PrefabTable sameOne(std::vector<PrefabTable::Entry>{{"Crate", 5}});
    const PrefabTable otherShape(std::vector<PrefabTable::Entry>{{"Crate", 6}});
    const PrefabTable otherName(std::vector<PrefabTable::Entry>{{"Barrel", 5}});

    EXPECT_EQ(HashRegistry(one), HashRegistry(sameOne));
    EXPECT_NE(HashRegistry(empty), HashRegistry(one));
    EXPECT_NE(HashRegistry(one), HashRegistry(otherShape));
    EXPECT_NE(HashRegistry(one), HashRegistry(otherName));
}

TEST(NetworkFingerprintTest, TheFirstDifferenceInOrderIsTheOneNamed)
{
    const Fingerprint server = SameFingerprint();
    EXPECT_EQ(CompareFingerprints(server, server), JoinRefusal::None);

    Fingerprint client = server;
    client.scenePath = "res://another/path.scene"; // not compared: a path says nothing about content
    EXPECT_EQ(CompareFingerprints(server, client), JoinRefusal::None);

    client = server;
    client.sceneObjectCount = 6;
    EXPECT_EQ(CompareFingerprints(server, client), JoinRefusal::ObjectCount);
    client.registryHash = 23;
    EXPECT_EQ(CompareFingerprints(server, client), JoinRefusal::Registry);
    client = server;
    client.prefabCount = 3;
    EXPECT_EQ(CompareFingerprints(server, client), JoinRefusal::Registry);
    client.sceneHash = 12;
    EXPECT_EQ(CompareFingerprints(server, client), JoinRefusal::SceneContent);
    client.engineVersion = "2.0.0";
    EXPECT_EQ(CompareFingerprints(server, client), JoinRefusal::EngineVersion);
}

TEST(NetworkFingerprintTest, EveryRefusalIsDescribedInWords)
{
    Fingerprint server = SameFingerprint();
    Fingerprint client = server;
    client.engineVersion = "2.0.0";
    client.sceneHash = 0xDEAD;
    client.scenePath = "res://client.scene";
    client.registryHash = 99;
    client.prefabCount = 4;
    client.sceneObjectCount = 8;

    const std::string engine = DescribeRefusal(JoinRefusal::EngineVersion, server, client);
    EXPECT_NE(engine.find("1.0.0"), std::string::npos);
    EXPECT_NE(engine.find("2.0.0"), std::string::npos);

    const std::string scene = DescribeRefusal(JoinRefusal::SceneContent, server, client);
    EXPECT_NE(scene.find("res://scenes/a.scene"), std::string::npos);
    EXPECT_NE(scene.find("res://client.scene"), std::string::npos);
    EXPECT_NE(scene.find("000000000000DEAD"), std::string::npos);
    EXPECT_NE(scene.find("scene"), std::string::npos);

    const std::string registry = DescribeRefusal(JoinRefusal::Registry, server, client);
    EXPECT_NE(registry.find("prefab"), std::string::npos);
    EXPECT_NE(registry.find("4"), std::string::npos);

    const std::string count = DescribeRefusal(JoinRefusal::ObjectCount, server, client);
    EXPECT_NE(count.find("5"), std::string::npos);
    EXPECT_NE(count.find("8"), std::string::npos);

    EXPECT_FALSE(DescribeRefusal(JoinRefusal::WireVersion, server, client).empty());
    for (const JoinRefusal refusal : {JoinRefusal::None, JoinRefusal::WireVersion, JoinRefusal::EngineVersion,
                                      JoinRefusal::SceneContent, JoinRefusal::Registry, JoinRefusal::ObjectCount})
    {
        EXPECT_NE(ToString(refusal), "unknown");
    }
}

TEST(PrefabTableTest, IsSortedByNameWithoutDuplicates)
{
    const PrefabTable table(std::vector<PrefabTable::Entry>{{"Zebra", 1}, {"Apple", 2}, {"Mango", 3}, {"Apple", 99}});
    ASSERT_EQ(table.Size(), 3u);
    EXPECT_EQ(table.At(0)->name, "Apple");
    EXPECT_EQ(table.At(0)->shapeHash, 2u); // the first of a repeated name is kept
    EXPECT_EQ(table.At(1)->name, "Mango");
    EXPECT_EQ(table.At(2)->name, "Zebra");
    EXPECT_EQ(table.At(3), nullptr);

    EXPECT_EQ(table.IndexOf("Apple"), 0);
    EXPECT_EQ(table.IndexOf("Mango"), 1);
    EXPECT_EQ(table.IndexOf("Zebra"), 2);
    EXPECT_FALSE(table.IndexOf("Banana").has_value());
    EXPECT_FALSE(table.IndexOf("").has_value());
    EXPECT_FALSE(table.IndexOf("apple").has_value());
}

TEST(PrefabTableTest, HoldsAtMostTheCapAndTheSameTableHashesTheSame)
{
    std::vector<PrefabTable::Entry> entries;
    for (int i = 0; i < kMaxPrefabs + 50; ++i)
    {
        char name[16];
        std::snprintf(name, sizeof name, "p%05d", i);
        entries.push_back({name, static_cast<std::uint64_t>(i)});
    }
    const PrefabTable table(entries);
    EXPECT_EQ(table.Size(), static_cast<std::size_t>(kMaxPrefabs));
    EXPECT_EQ(table.At(0)->name, "p00000");
    EXPECT_EQ(table.IndexOf("p00000"), 0);
    EXPECT_FALSE(table.IndexOf("p01100").has_value());

    std::reverse(entries.begin(), entries.end());
    EXPECT_EQ(PrefabTable(entries).Hash(), table.Hash());
    EXPECT_EQ(PrefabTable().Size(), 0u);
}

TEST(PrefabTableTest, ShapeHashSeesStructureNotIdentity)
{
    const ScopedPrefab first("FingerprintTest_ShapeA");
    const ScopedPrefab second("FingerprintTest_ShapeB"); // the same structure; every GameObject has its own random UUID
    const ScopedPrefab other("FingerprintTest_ShapeC", ScopedPrefab::Shape::None);
    const ScopedPrefab onChild("FingerprintTest_ShapeD", ScopedPrefab::Shape::OneOnChild);
    ASSERT_TRUE(first.Registered() && second.Registered() && other.Registered() && onChild.Registered());

    const auto firstData = PrefabManager::GetPrefabData(first.Name());
    const auto secondData = PrefabManager::GetPrefabData(second.Name());
    const auto otherData = PrefabManager::GetPrefabData(other.Name());
    const auto childData = PrefabManager::GetPrefabData(onChild.Name());
    ASSERT_TRUE(firstData && secondData && otherData && childData);

    // The prefab name is in the Prefab JSON but the shape starts at the root object; the roots are named after the
    // prefab, so they differ by name: compare shapes of two prefabs that are the same but for it
    nlohmann::json a = *firstData;
    nlohmann::json b = *secondData;
    a["rootObject"]["name"] = "Same";
    b["rootObject"]["name"] = "Same";
    EXPECT_NE(a["rootObject"]["uuid"], b["rootObject"]["uuid"]);
    EXPECT_EQ(PrefabShapeHash(a), PrefabShapeHash(b));

    EXPECT_NE(PrefabShapeHash(*firstData), PrefabShapeHash(*otherData));
    EXPECT_NE(PrefabShapeHash(*firstData), PrefabShapeHash(*childData));

    // A component moved from the root to the child is another shape
    nlohmann::json c = *childData;
    c["rootObject"]["name"] = "Same";
    EXPECT_NE(PrefabShapeHash(a), PrefabShapeHash(c));

    // The bare JSON of the root hashes as the wrapped prefab does
    EXPECT_EQ(PrefabShapeHash(a), PrefabShapeHash(a["rootObject"]));

    // Anything else is a shape too, and never a crash
    EXPECT_NO_THROW({ (void)PrefabShapeHash(nlohmann::json()); });
    EXPECT_NO_THROW({ (void)PrefabShapeHash(nlohmann::json::array({1, 2, 3})); });
}

TEST(PrefabTableTest, CaptureTakesTheRegisteredPrefabsAndIsFrozen)
{
    const ScopedPrefab b("FingerprintTest_CaptureB");
    const ScopedPrefab a("FingerprintTest_CaptureA");
    ASSERT_TRUE(a.Registered() && b.Registered());

    const std::vector<std::string> names = PrefabManager::GetRegisteredPrefabNames();
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    EXPECT_TRUE(std::find(names.begin(), names.end(), a.Name()) != names.end());

    const PrefabTable table = PrefabTable::Capture();
    const auto indexA = table.IndexOf(a.Name());
    const auto indexB = table.IndexOf(b.Name());
    ASSERT_TRUE(indexA.has_value());
    ASSERT_TRUE(indexB.has_value());
    EXPECT_LT(*indexA, *indexB);
    EXPECT_EQ(table.At(*indexA)->name, a.Name());
    EXPECT_EQ(PrefabTable::Capture().Hash(), table.Hash());

    // A prefab registered after the capture is not in it
    const std::uint64_t before = table.Hash();
    {
        const ScopedPrefab late("FingerprintTest_CaptureLate");
        EXPECT_FALSE(table.IndexOf(late.Name()).has_value());
        EXPECT_NE(PrefabTable::Capture().Hash(), before);
    }
    EXPECT_EQ(PrefabTable::Capture().Hash(), before);
    EXPECT_EQ(table.Hash(), before);
}

TEST(PrefabTableTest, PrefabManagerReportsWhatIsRegistered)
{
    EXPECT_FALSE(PrefabManager::GetPrefabData("FingerprintTest_Nothing").has_value());
    const ScopedPrefab prefab("FingerprintTest_Reported");
    ASSERT_TRUE(prefab.Registered());
    const auto data = PrefabManager::GetPrefabData(prefab.Name());
    ASSERT_TRUE(data.has_value());
    EXPECT_TRUE(data->contains("rootObject"));
    EXPECT_EQ((*data)["name"], prefab.Name());
}
