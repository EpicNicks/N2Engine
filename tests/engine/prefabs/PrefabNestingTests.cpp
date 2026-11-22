#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/prefabs/PrefabNesting.hpp"
#include "engine/serialization/ComponentRegistry.hpp"
#include "engine/serialization/ComponentSerializer.hpp"
#include "engine/serialization/ReferenceResolver.hpp"

// Nested prefabs: links in scene and prefab JSON, expanded and collapsed (orchestrator/proposals/prefab-nesting.md,
// docs/serialization.html "Prefab links")

using namespace N2Engine;
using json = nlohmann::json;
namespace Nesting = N2Engine::PrefabNesting;

namespace
{
    class NestValue final : public SerializableComponent
    {
    public:
        explicit NestValue(GameObject &go) : SerializableComponent(go)
        {
            RegisterMember("value", value);
            RegisterGameObjectRef("target", target);
        }
        [[nodiscard]] std::string GetTypeName() const override { return "NestTest_Value"; }
        int value = 0;
        GameObject *target = nullptr;
    };

    void RegisterNestComponents()
    {
        ComponentRegistry::Instance().Register(
            "NestTest_Value", [](GameObject &go) -> std::unique_ptr<Component> { return std::make_unique<NestValue>(go); });
    }

    // Fixed ids, so the expected derived ids can be computed in the tests
    std::string Id(const int n)
    {
        return Math::UUID::GenerateNameBased(Math::UUID::ZERO, "nest-test-" + std::to_string(n)).ToString();
    }

    std::string Derive(const std::string &link, const std::string &source)
    {
        return Nesting::DeriveId(*Math::UUID::FromString(link), *Math::UUID::FromString(source)).ToString();
    }

    json Value(const std::string &uuid, const int value, const json &target = nullptr)
    {
        return {{"type", "NestTest_Value"}, {"data", {{"uuid", uuid}, {"isActive", true}, {"value", value}, {"target", target}}}};
    }

    json Object(const std::string &uuid, const std::string &name, json components = json::array(),
                json children = json::array())
    {
        return {{"uuid", uuid},
                {"name", name},
                {"tag", "Untagged"},
                {"layer", 0},
                {"isActive", true},
                {"positionable",
                 {{"localPosition", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
                  {"localRotation", {{"w", 1.0}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
                  {"localScale", {{"x", 1.0}, {"y", 1.0}, {"z", 1.0}}}}},
                {"components", std::move(components)},
                {"children", std::move(children)}};
    }

    json Link(const std::string &uuid, const std::string &name, const std::string &prefab, json instance = json::object())
    {
        instance["prefab"] = prefab;
        return {{"uuid", uuid},
                {"name", name},
                {"positionable",
                 {{"localPosition", {{"x", 3.0}, {"y", 4.0}, {"z", 0.0}}},
                  {"localRotation", {{"w", 1.0}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}}}}},
                {"prefabInstance", std::move(instance)}};
    }

    class FakeSource final : public Nesting::PrefabSource
    {
    public:
        std::unordered_map<std::string, json> roots;
        int loads = 0;
        const json *LoadRoot(const Math::UUID &prefab) override
        {
            ++loads;
            const auto it = roots.find(prefab.ToString());
            return it == roots.end() ? nullptr : &it->second;
        }
    };

    // Prefab "Crate" (id 100): root 101 with a Value (102, value 1, target -> its child), child "Lid" 103 with a
    // Value 104 (value 2)
    const std::string CratePrefab = Id(100);
    json CrateRoot()
    {
        return Object(Id(101), "Crate", json::array({Value(Id(102), 1, Id(103))}),
                      json::array({Object(Id(103), "Lid", json::array({Value(Id(104), 2)}))}));
    }

    // Prefab "Room" (id 200): root 201 with a child link "Crate A" (file id 202) to Crate
    const std::string RoomPrefab = Id(200);
    json RoomRoot()
    {
        return Object(Id(201), "Room", json::array(),
                      json::array({Link(Id(202), "Crate A", CratePrefab,
                                        {{"overrides", json::array({{{"source", Id(103)}, {"path", "name"}, {"value", "Big lid"}}})}})}));
    }

    FakeSource Source()
    {
        FakeSource source;
        source.roots[CratePrefab] = CrateRoot();
        source.roots[RoomPrefab] = RoomRoot();
        return source;
    }

    json Expanded(json link, FakeSource &source, Nesting::ExpandReport *reportOut = nullptr)
    {
        Nesting::ExpandReport report;
        Nesting::Expand(link, source, report);
        if (reportOut != nullptr)
        {
            *reportOut = report;
        }
        return link;
    }

    // Expanded data as the live objects write it back (what the editor would collapse)
    json Rebuilt(const json &objectJson)
    {
        ReferenceResolver resolver;
        const auto built = GameObject::Deserialize(objectJson, &resolver);
        resolver.ResolveAll();
        return built->Serialize();
    }
}

TEST(PrefabNestingTest, ExpandsALinkWithDerivedIdsAndMarks)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const std::string link = Id(1);
    const json out = Expanded(Link(link, "My crate", CratePrefab), source);

    EXPECT_FALSE(out.contains("prefabInstance"));
    EXPECT_EQ(out["uuid"], link) << "the root takes the link's id";
    EXPECT_EQ(out["name"], "My crate") << "and its name";
    EXPECT_EQ(out["positionable"]["localPosition"]["x"], 3.0) << "and its position";
    EXPECT_EQ(out["prefabLinked"], true);
    EXPECT_EQ(out["prefabReference"], CratePrefab);
    EXPECT_EQ(out["components"][0]["data"]["uuid"], Derive(link, Id(102)));
    EXPECT_EQ(out["components"][0]["prefabSource"], Id(102));

    const json &lid = out["children"][0];
    EXPECT_EQ(lid["uuid"], Derive(link, Id(103)));
    EXPECT_EQ(lid["prefabLink"], link);
    EXPECT_EQ(lid["prefabSource"], Id(103));
    EXPECT_EQ(lid["components"][0]["data"]["uuid"], Derive(link, Id(104)));
    EXPECT_EQ(out["components"][0]["data"]["target"], Derive(link, Id(103))) << "a reference inside the prefab follows";
}

TEST(PrefabNestingTest, SameFileSameIdsAndTwoLinksNeverCollide)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const json a = Expanded(Link(Id(1), "A", CratePrefab), source);
    const json again = Expanded(Link(Id(1), "A", CratePrefab), source);
    const json b = Expanded(Link(Id(2), "B", CratePrefab), source);
    EXPECT_EQ(a, again);
    EXPECT_NE(a["children"][0]["uuid"], b["children"][0]["uuid"]);
    EXPECT_NE(a["components"][0]["data"]["uuid"], b["components"][0]["data"]["uuid"]);
}

TEST(PrefabNestingTest, OverridesRemovalsAndAdditions)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const std::string link = Id(1);
    const json instance = {
        {"overrides",
         json::array({{{"source", Id(103)}, {"path", "name"}, {"value", "Open lid"}},
                      {{"source", Id(101)}, {"path", "positionable.localScale"}, {"value", {{"x", 2.0}, {"y", 2.0}, {"z", 1.0}}}},
                      {{"source", Id(101)}, {"component", Id(102)}, {"path", "value"}, {"value", 7}},
                      {{"source", Id(101)}, {"path", "name"}, {"value", "ignored: the root's name is the link's"}}})},
        {"removedComponents", json::array({Id(104)})},
        {"addedComponents", json::array({{{"source", Id(103)}, {"component", Value(Id(9), 9)}}})},
        {"addedChildren", json::array({{{"source", Id(101)}, {"object", Object(Id(8), "Sticker")}}})}};
    const json out = Expanded(Link(link, "My crate", CratePrefab, instance), source);

    EXPECT_EQ(out["name"], "My crate");
    EXPECT_EQ(out["positionable"]["localScale"]["x"], 2.0);
    EXPECT_EQ(out["components"][0]["data"]["value"], 7);
    const json &lid = out["children"][0];
    EXPECT_EQ(lid["name"], "Open lid");
    ASSERT_EQ(lid["components"].size(), 1u) << "104 removed, 9 added";
    EXPECT_EQ(lid["components"][0]["data"]["uuid"], Id(9)) << "an added component keeps its own id";
    EXPECT_FALSE(lid["components"][0].contains("prefabSource"));
    ASSERT_EQ(out["children"].size(), 2u);
    EXPECT_EQ(out["children"][1]["uuid"], Id(8));
    EXPECT_FALSE(out["children"][1].contains("prefabLink")) << "an added child is the instance's own";
}

TEST(PrefabNestingTest, NestedLinksAndPathsThroughThem)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const std::string room = Id(1);
    const json instance = {
        {"overrides", json::array({{{"source", Id(202) + "/" + Id(103)}, {"component", Id(202) + "/" + Id(104)},
                                    {"path", "value"}, {"value", 42}}})}};
    Nesting::ExpandReport report;
    const json out = Expanded(Link(room, "Room 1", RoomPrefab, instance), source, &report);
    EXPECT_EQ(report.links, 2u);
    EXPECT_EQ(report.errors, 0u);

    const json &crate = out["children"][0];
    const std::string crateId = Derive(room, Id(202));
    EXPECT_EQ(crate["uuid"], crateId);
    EXPECT_EQ(crate["name"], "Crate A") << "the inner link's own name";
    EXPECT_EQ(crate["prefabLinked"], true);
    EXPECT_EQ(crate["prefabReference"], CratePrefab);
    EXPECT_EQ(crate["prefabLink"], room);
    EXPECT_EQ(crate["prefabSource"], Id(202));

    const json &lid = crate["children"][0];
    EXPECT_EQ(lid["uuid"], Derive(crateId, Id(103))) << "id(x) = Derive(id(its link root), its source)";
    EXPECT_EQ(lid["prefabLink"], crateId);
    EXPECT_EQ(lid["prefabSource"], Id(103));
    EXPECT_EQ(lid["name"], "Big lid") << "the room prefab's own override";
    EXPECT_EQ(lid["components"][0]["data"]["value"], 42) << "an override two links deep";
    EXPECT_EQ(lid["components"][0]["data"]["uuid"], Derive(crateId, Id(104)));
}

TEST(PrefabNestingTest, AVariantIsAPrefabWhoseRootIsALink)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const std::string variant = Id(300);
    source.roots[variant] = Link(Id(301), "Gold crate", CratePrefab,
                                 {{"overrides", json::array({{{"source", Id(101)}, {"component", Id(102)}, {"path", "value"}, {"value", 5}}})}});
    const std::string link = Id(1);
    const json out = Expanded(Link(link, "Gold 1", variant), source);
    EXPECT_EQ(out["uuid"], link);
    EXPECT_EQ(out["prefabReference"], variant) << "an instance of the variant";
    EXPECT_EQ(out["components"][0]["data"]["value"], 5);
    const json &lid = out["children"][0];
    EXPECT_EQ(lid["prefabLink"], link);
    EXPECT_EQ(lid["uuid"], Derive(link, lid["prefabSource"].get<std::string>()));
}

TEST(PrefabNestingTest, CyclesMissingPrefabsAndStaleChangesFailSoftly)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const std::string loop = Id(400);
    source.roots[loop] = Object(Id(401), "Loop", json::array(), json::array({Link(Id(402), "Again", loop)}));

    Nesting::ExpandReport report;
    json out;
    ASSERT_NO_THROW(out = Expanded(Link(Id(1), "Loop 1", loop), source, &report));
    EXPECT_EQ(report.errors, 1u);
    EXPECT_EQ(out["children"][0]["name"], "Again") << "the cyclic link became an empty placeholder";
    EXPECT_TRUE(out["children"][0]["children"].empty());

    ASSERT_NO_THROW(out = Expanded(Link(Id(2), "Gone", Id(999)), source, &report));
    EXPECT_EQ(report.errors, 1u);
    EXPECT_EQ(out["uuid"], Id(2));
    EXPECT_EQ(out["name"], "Gone");
    EXPECT_TRUE(out["components"].empty());

    ASSERT_NO_THROW(out = Expanded(Link(Id(3), "Stale", CratePrefab,
                                        {{"overrides", json::array({{{"source", Id(998)}, {"path", "name"}, {"value", "x"}}})},
                                         {"removedObjects", json::array({Id(997)})}}),
                                   source, &report));
    EXPECT_EQ(report.errors, 0u);
    EXPECT_EQ(report.droppedChanges, 2u);
    EXPECT_EQ(out["children"].size(), 1u);
}

TEST(PrefabNestingTest, CollapseIsTheInverseOfExpand)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const json instance = {
        {"overrides", json::array({{{"source", Id(103)}, {"path", "name"}, {"value", "Open lid"}},
                                   {{"source", Id(101)}, {"component", Id(102)}, {"path", "value"}, {"value", 7}}})},
        {"removedComponents", json::array({Id(104)})},
        {"addedChildren", json::array({{{"source", Id(101)}, {"object", Object(Id(8), "Sticker")}}})}};
    const json link = Link(Id(1), "My crate", CratePrefab, instance);
    const json live = Rebuilt(Expanded(link, source));

    json collapsed = live;
    ASSERT_TRUE(Nesting::Collapse(collapsed, source));
    ASSERT_TRUE(collapsed.contains("prefabInstance"));
    EXPECT_EQ(collapsed["uuid"], Id(1));
    EXPECT_EQ(collapsed["name"], "My crate");
    const json &got = collapsed["prefabInstance"];
    EXPECT_EQ(got["prefab"], CratePrefab);
    ASSERT_EQ(got["overrides"].size(), 2u) << got.dump();
    EXPECT_EQ(got["removedComponents"], json::array({Id(104)}));
    ASSERT_EQ(got["addedChildren"].size(), 1u);
    EXPECT_EQ(got["addedChildren"][0]["source"], Id(101));

    // Expanding what Collapse wrote gives the same objects again
    EXPECT_EQ(Rebuilt(Expanded(collapsed, source)), live);

    // An untouched instance collapses to a bare link
    json untouched = Rebuilt(Expanded(Link(Id(2), "Plain", CratePrefab), source));
    ASSERT_TRUE(Nesting::Collapse(untouched, source));
    EXPECT_EQ(untouched["prefabInstance"], json({{"prefab", CratePrefab}}));
}

TEST(PrefabNestingTest, CollapseFindsEditsInsideANestedLink)
{
    RegisterNestComponents();
    FakeSource source = Source();
    json live = Rebuilt(Expanded(Link(Id(1), "Room 1", RoomPrefab), source));
    live["children"][0]["children"][0]["components"][0]["data"]["value"] = 13; // the room's crate's lid

    json collapsed = live;
    ASSERT_TRUE(Nesting::Collapse(collapsed, source));
    const json &overrides = collapsed["prefabInstance"]["overrides"];
    ASSERT_EQ(overrides.size(), 1u) << collapsed.dump();
    EXPECT_EQ(overrides[0]["source"], Id(202) + "/" + Id(103));
    EXPECT_EQ(overrides[0]["component"], Id(202) + "/" + Id(104));
    EXPECT_EQ(overrides[0]["value"], 13);
    EXPECT_EQ(Rebuilt(Expanded(collapsed, source)), live);
}

TEST(PrefabNestingTest, CollapseLeavesAnInstanceExpandedWhenItsPrefabIsGone)
{
    RegisterNestComponents();
    FakeSource source = Source();
    json live = Rebuilt(Expanded(Link(Id(1), "My crate", CratePrefab), source));
    FakeSource empty;
    json kept = live;
    EXPECT_FALSE(Nesting::Collapse(kept, empty));
    EXPECT_EQ(kept, live) << "nothing is lost";
}

TEST(PrefabNestingTest, ACopyOfAnInstanceDerivesItsIdsFromItsOwnLinkId)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const json expanded = Expanded(Link(Id(1), "Room 1", RoomPrefab), source);
    const auto copies = PrefabManager::InstantiatePrefabs({&expanded}, nullptr);
    ASSERT_EQ(copies.size(), 1u);
    const json copy = copies.front()->Serialize();

    const std::string root = copy["uuid"];
    EXPECT_NE(root, Id(1)) << "a copy gets a fresh link id";
    const json &crate = copy["children"][0];
    EXPECT_EQ(crate["uuid"], Derive(root, Id(202)));
    EXPECT_EQ(crate["prefabLink"], root);
    const json &lid = crate["children"][0];
    EXPECT_EQ(lid["uuid"], Derive(crate["uuid"], Id(103)));
    EXPECT_EQ(lid["prefabSource"], Id(103)) << "sources name prefab ids and are never renumbered";
    EXPECT_EQ(crate["components"][0]["data"]["target"], lid["uuid"]) << "references follow into the copy";

    // A part of an instance copied alone is plain objects: its marks name a link root outside the copy
    const json &part = expanded["children"][0]["children"][0];
    const auto parts = PrefabManager::InstantiatePrefabs({&part}, nullptr);
    ASSERT_EQ(parts.size(), 1u);
    const json plain = parts.front()->Serialize();
    EXPECT_FALSE(plain.contains("prefabLink"));
    EXPECT_FALSE(plain.contains("prefabSource"));
}

TEST(PrefabNestingTest, RuntimeSerializeWritesExpandedCopiesWithMarks)
{
    RegisterNestComponents();
    FakeSource source = Source();
    const json live = Rebuilt(Expanded(Link(Id(1), "My crate", CratePrefab), source));
    EXPECT_FALSE(live.contains("prefabInstance"));
    EXPECT_EQ(live["prefabLinked"], true);
    EXPECT_EQ(live["prefabReference"], CratePrefab);
    EXPECT_EQ(live["children"][0]["prefabSource"], Id(103));
    EXPECT_FALSE(Nesting::ContainsLinks(live)) << "an expanded copy loads as plain objects";
    EXPECT_TRUE(Nesting::ContainsLinks(json::array({Object(Id(5), "x"), Link(Id(6), "y", CratePrefab)})));
}

TEST(PrefabNestingTest, InstanceAsPrefabWritesTheInstanceInThePrefabsOwnIds)
{
    RegisterNestComponents();
    FakeSource source = Source();
    json live = Rebuilt(Expanded(Link(Id(1), "Room 1", RoomPrefab), source));
    live["children"][0]["name"] = "Crate B";                    // the inner link's own name
    live["children"].push_back(Object(Id(9), "Added"));          // an object the instance added

    const auto content = Nesting::InstanceAsPrefab(live, source);
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ((*content)["uuid"], Id(201)) << "the root gets back the prefab's root id";
    EXPECT_FALSE(content->contains("prefabReference"));
    EXPECT_FALSE(content->contains("prefabLinked"));
    const json &inner = (*content)["children"][0];
    EXPECT_EQ(inner["uuid"], Id(202)) << "the link inside keeps its id in the prefab";
    EXPECT_EQ(inner["name"], "Crate B");
    ASSERT_TRUE(inner.contains("prefabInstance")) << "and is written as a link";
    EXPECT_EQ(inner["prefabInstance"]["prefab"], CratePrefab);
    EXPECT_FALSE(inner.contains("children"));
    EXPECT_EQ((*content)["children"][1]["uuid"], Id(9));

    // As a prefab again, it expands to what the instance shows
    FakeSource after = Source();
    after.roots[RoomPrefab] = *content;
    const json again = Rebuilt(Expanded(Link(Id(1), "Room 1", RoomPrefab), after));
    EXPECT_EQ(again["children"][0]["name"], "Crate B");
    EXPECT_EQ(again["children"][0]["children"][0]["name"], "Big lid") << "the inner link's overrides are kept";
    EXPECT_EQ(again["children"][1]["name"], "Added");
}

TEST(PrefabNestingTest, ResolveSourcePathFollowsTheIdsAnExpansionDerives)
{
    const std::string link = Math::UUID::Random().ToString();
    const std::string root = Math::UUID::Random().ToString();
    const std::string a = Math::UUID::Random().ToString();
    const std::string b = Math::UUID::Random().ToString();
    const auto uuid = [](const std::string &text) { return *Math::UUID::FromString(text); };

    EXPECT_EQ(Nesting::ResolveSourcePath(link, root, root), link) << "the prefab's root is the link root";
    EXPECT_EQ(Nesting::ResolveSourcePath(link, root, a), Nesting::DeriveId(uuid(link), uuid(a)).ToString());
    EXPECT_EQ(Nesting::ResolveSourcePath(link, root, a + "/" + b),
              Nesting::DeriveId(Nesting::DeriveId(uuid(link), uuid(a)), uuid(b)).ToString())
        << "through a link inside: its root's id, then the object's inside it";
    EXPECT_FALSE(Nesting::ResolveSourcePath(link, root, "not an id").has_value());
    EXPECT_FALSE(Nesting::ResolveSourcePath(link, root, a + "/").has_value());
}

TEST(PrefabNestingTest, RenumberIdsRewritesIdsEverywhereButPrefabSources)
{
    const std::string from = Math::UUID::Random().ToString();
    const std::string to = Math::UUID::Random().ToString();
    json value = {{"uuid", from},
                  {"prefabSource", from},
                  {"components", json::array({{{"data", {{"target", from}}}, {"prefabSource", from}}})},
                  {"other", "text"}};
    Nesting::RenumberIds(value, {{from, to}});
    EXPECT_EQ(value["uuid"], to);
    EXPECT_EQ(value["components"][0]["data"]["target"], to);
    EXPECT_EQ(value["prefabSource"], from) << "an id in a prefab stays as it is";
    EXPECT_EQ(value["components"][0]["prefabSource"], from);
    EXPECT_EQ(value["other"], "text");
}
