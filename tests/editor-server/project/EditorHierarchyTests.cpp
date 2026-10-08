#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// The hierarchy and entity commands (#76, E4) through EditorServer::ExecuteCommand, on a scene made with NewScene.
// With the project tests, a program of its own: these tests load scenes into SceneManager, which the other
// editor-server tests expect to be empty.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t HierarchyType = static_cast<uint8_t>(ResponseType::Hierarchy);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);

    constexpr const char *NoParent = "";
    constexpr int32_t Last = -1;

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return {payload.begin(), payload.end()}; }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const auto body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    std::vector<uint8_t> Strings(std::initializer_list<std::string> values)
    {
        BufferWriter w;
        for (const std::string &value : values)
            w.WriteString(value);
        return w.Release();
    }

    /// What GetEntity answers
    struct EntityData
    {
        json entity;
        std::array<float, 16> worldMatrix{};
    };

    /// A component with a reference to a GameObject, to see what a copy does with it
    class HierarchyTestHolder final : public SerializableComponent
    {
    public:
        explicit HierarchyTestHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterGameObjectRef("target", target);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "EditorHierarchyTest_Holder"; }

        GameObject *target = nullptr;
    };

    /// Counts the OnEnable and OnDisable calls it gets (in statics: a component is made by AddComponent, from a
    /// constructor taking only its object)
    class HierarchyTestProbe final : public Component
    {
    public:
        explicit HierarchyTestProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "EditorHierarchyTest_Probe"; }

        void OnEnable() override { ++enableCalls; }
        void OnDisable() override { ++disableCalls; }

        static inline int enableCalls = 0;
        static inline int disableCalls = 0;
    };

    void RegisterHolder()
    {
        ComponentRegistry::Instance().Register(
            "EditorHierarchyTest_Holder",
            [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<HierarchyTestHolder>(gameObject); });
    }

    /// Every sceneChanged event the server pushed
    std::vector<json> SceneChangedEvents(const EditorServer &server)
    {
        std::vector<json> found;
        for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
        {
            if (event.value("kind", "") == "sceneChanged")
                found.push_back(event);
        }
        return found;
    }

    /// A scene made by NewScene (no project needed), and helpers for the commands
    class EditorHierarchyTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Hierarchy Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
        }

        std::string Create(const std::string &name, const std::string &parentId = NoParent, int32_t siblingIndex = Last,
                           const std::string &preset = "")
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString(parentId);
            w.WriteI32(siblingIndex);
            w.WriteString(preset);
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        Frame Reparent(const std::string &id, const std::string &parentId, int32_t siblingIndex, bool keepWorld = true)
        {
            BufferWriter w;
            w.WriteString(id);
            w.WriteString(parentId);
            w.WriteI32(siblingIndex);
            w.WriteBool(keepWorld);
            return Execute(server, CommandType::SetEntityParent, w.Release());
        }

        Frame SetProperties(const std::string &id, const json &properties)
        {
            BufferWriter w;
            w.WriteString(id);
            WriteJson(w, properties);
            return Execute(server, CommandType::SetEntityProperties, w.Release());
        }

        Frame SetLocal(const std::string &id, const std::array<float, 3> &position, const std::array<float, 4> &rotation,
                       const std::array<float, 3> &scale = {1.0f, 1.0f, 1.0f})
        {
            BufferWriter w;
            w.WriteString(id);
            for (const float v : position)
                w.WriteF32(v);
            for (const float v : rotation) // x, y, z, w
                w.WriteF32(v);
            for (const float v : scale)
                w.WriteF32(v);
            return Execute(server, CommandType::SetLocalTransform, w.Release());
        }

        Frame Duplicate(const std::string &id) { return Execute(server, CommandType::DuplicateEntity, Strings({id})); }

        std::string DuplicateId(const std::string &id)
        {
            const Frame copy = Duplicate(id);
            EXPECT_EQ(copy.type, EntityCreatedType) << copy.Text();
            if (copy.type != EntityCreatedType)
                return {};
            BufferReader r(copy.payload);
            return r.ReadString();
        }

        /// GetHierarchy's nodes (depth first); revision, when asked for, is the one it was read at
        json Nodes(uint32_t *revision = nullptr)
        {
            const Frame frame = Execute(server, CommandType::GetHierarchy);
            EXPECT_EQ(frame.type, HierarchyType) << frame.Text();
            if (frame.type != HierarchyType)
                return json::array();
            BufferReader r(frame.payload);
            const uint32_t read = r.ReadU32();
            if (revision != nullptr)
                *revision = read;
            const json nodes = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return nodes;
        }

        std::vector<std::string> Names()
        {
            std::vector<std::string> names;
            for (const json &node : Nodes())
                names.push_back(node.at("name").get<std::string>());
            return names;
        }

        json Node(const std::string &id)
        {
            for (const json &node : Nodes())
            {
                if (node.at("id") == id)
                    return node;
            }
            ADD_FAILURE() << "no node " << id;
            return json::object();
        }

        EntityData GetEntity(const std::string &id)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({id}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            EntityData data;
            if (frame.type != EntityDataType)
                return data;
            BufferReader r(frame.payload);
            data.entity = ReadJson(r);
            for (float &element : data.worldMatrix)
                element = r.ReadF32();
            EXPECT_FALSE(r.HasData());
            return data;
        }

        uint32_t Revision()
        {
            const Frame frame = Execute(server, CommandType::GetOpenScene);
            EXPECT_EQ(frame.type, SceneInfoType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadString();
            (void)r.ReadString();
            (void)r.ReadString();
            return r.ReadU32();
        }

        [[nodiscard]] static Scene &LoadedScene() { return SceneManager::GetCurSceneRef(); }

        EditorServer server;
    };
}

// ==================== GetHierarchy ====================

TEST_F(EditorHierarchyTest, AnEmptySceneHasAnEmptyHierarchy)
{
    uint32_t revision = 12345;
    const json nodes = Nodes(&revision);
    EXPECT_TRUE(nodes.is_array());
    EXPECT_TRUE(nodes.empty());
    EXPECT_EQ(revision, Revision());
}

TEST_F(EditorHierarchyTest, TheHierarchyIsFlatDepthFirstWithParentsBeforeChildren)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    const std::string c = Create("C", a);
    const std::string d = Create("D", a);
    const std::string e = Create("E", c, Last, "Cube");

    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "C", "E", "D", "B"}));

    const json nodes = Nodes();
    ASSERT_EQ(nodes.size(), 5u);
    EXPECT_EQ(nodes[0].at("id"), a);
    EXPECT_EQ(nodes[0].at("parentId"), "");
    EXPECT_EQ(nodes[0].at("index"), 0u);
    EXPECT_EQ(Node(b).at("index"), 1u) << "the second root";
    EXPECT_EQ(Node(c).at("parentId"), a);
    EXPECT_EQ(Node(c).at("index"), 0u);
    EXPECT_EQ(Node(d).at("parentId"), a);
    EXPECT_EQ(Node(d).at("index"), 1u);

    const json node = Node(e);
    EXPECT_EQ(node.at("parentId"), c);
    EXPECT_EQ(node.at("name"), "E");
    EXPECT_EQ(node.at("active"), true);
    EXPECT_EQ(node.at("activeInHierarchy"), true);
    EXPECT_EQ(node.at("layer"), 0);
    EXPECT_EQ(node.at("tag"), "Untagged");
    EXPECT_EQ(node.at("components"), (json::array({"CubeRenderer"})));
    EXPECT_TRUE(Node(a).at("components").empty());
}

TEST_F(EditorHierarchyTest, TheHierarchyCarriesTheRevisionItWasReadAt)
{
    uint32_t before = 0;
    (void)Nodes(&before);
    EXPECT_EQ(before, Revision());

    (void)Create("A");
    uint32_t after = 0;
    (void)Nodes(&after);
    EXPECT_GT(after, before);
    EXPECT_EQ(after, Revision());

    // Reading changes nothing
    uint32_t again = 0;
    (void)Nodes(&again);
    (void)GetEntity(Nodes()[0].at("id").get<std::string>());
    EXPECT_EQ(again, after);
    EXPECT_EQ(Revision(), after);
}

TEST_F(EditorHierarchyTest, ActiveInHierarchyFollowsTheAncestors)
{
    const std::string parent = Create("Parent");
    const std::string child = Create("Child", parent);
    ASSERT_EQ(SetProperties(parent, json{{"active", false}}).type, OkType);

    const json node = Node(child);
    EXPECT_EQ(node.at("active"), true) << "its own flag";
    EXPECT_EQ(node.at("activeInHierarchy"), false) << "off under an inactive parent";
    EXPECT_EQ(Node(parent).at("active"), false);
}

// ==================== CreateEntityEx ====================

TEST_F(EditorHierarchyTest, PresetsAddTheirComponentsAndAlwaysATransform)
{
    struct Case
    {
        const char *preset;
        const char *name;
        const char *component; // nullptr: none
    };
    for (const Case &c : {Case{"", "GameObject", nullptr}, Case{"Empty", "GameObject", nullptr},
                          Case{"Cube", "Cube", "CubeRenderer"}, Case{"Sphere", "Sphere", "SphereRenderer"},
                          Case{"Quad", "Quad", "QuadRenderer"}, Case{"Light", "Light", "Light"},
                          Case{"DirectionalLight", "Directional Light", "Light"},
                          Case{"PointLight", "Point Light", "Light"}, Case{"SpotLight", "Spot Light", "Light"}})
    {
        SCOPED_TRACE(c.preset);
        const std::string id = Create("", NoParent, Last, c.preset);
        ASSERT_FALSE(id.empty());
        const json node = Node(id);
        EXPECT_EQ(node.at("name"), c.name) << "an empty name is the preset's";
        if (c.component == nullptr)
            EXPECT_TRUE(node.at("components").empty());
        else
            EXPECT_EQ(node.at("components"), (json::array({c.component})));

        const EntityData data = GetEntity(id);
        ASSERT_TRUE(data.entity.contains("transform")) << "an editor object always has a transform";
        EXPECT_EQ(data.entity.at("transform").at("scale").at("x"), 1.0);
    }
}

TEST_F(EditorHierarchyTest, TheLightPresetsAreOfTheirType)
{
    const std::vector<std::pair<std::string, std::string>> presets = {
        {"Light", "Directional"}, {"DirectionalLight", "Directional"}, {"PointLight", "Point"}, {"SpotLight", "Spot"}};
    for (const auto &[preset, type] : presets)
    {
        SCOPED_TRACE(preset);
        const EntityData data = GetEntity(Create("", NoParent, Last, preset));
        const json &components = data.entity.at("components");
        ASSERT_EQ(components.size(), 1u);
        EXPECT_EQ(components[0].at("type"), "Light");
        EXPECT_EQ(components[0].at("values").at("type"), type);
    }
}

TEST_F(EditorHierarchyTest, ANameGivenIsUsedAndAParentTakesTheChild)
{
    const std::string parent = Create("Parent");
    const std::string child = Create("Mine", parent, Last, "Sphere");
    EXPECT_EQ(Node(child).at("name"), "Mine");
    EXPECT_EQ(Node(child).at("parentId"), parent);
}

TEST_F(EditorHierarchyTest, TheSiblingIndexPlacesTheNewObject)
{
    (void)Create("A");
    (void)Create("B");
    (void)Create("C");

    (void)Create("First", NoParent, 0);
    EXPECT_EQ(Names(), (std::vector<std::string>{"First", "A", "B", "C"}));
    (void)Create("Middle", NoParent, 2);
    EXPECT_EQ(Names(), (std::vector<std::string>{"First", "A", "Middle", "B", "C"}));
    (void)Create("Past", NoParent, 99);
    EXPECT_EQ(Names().back(), "Past") << "past the last is the last";
    (void)Create("Default", NoParent, Last);
    EXPECT_EQ(Names().back(), "Default");

    const std::string parent = Create("P");
    (void)Create("X", parent);
    (void)Create("Y", parent, 0);
    const json nodes = Nodes();
    std::vector<std::string> under;
    for (const json &node : nodes)
    {
        if (node.at("parentId") == parent)
            under.push_back(node.at("name"));
    }
    EXPECT_EQ(under, (std::vector<std::string>{"Y", "X"}));
}

TEST_F(EditorHierarchyTest, ARefusedCreateMakesNothingAndMovesNothing)
{
    (void)Create("A");
    const uint32_t revision = Revision();
    const size_t count = Nodes().size();

    EXPECT_EQ(Execute(server, CommandType::CreateEntityEx, [&]
    {
        BufferWriter w;
        w.WriteString("X");
        w.WriteString("00000000-0000-0000-0000-000000000000");
        w.WriteI32(Last);
        w.WriteString("");
        return w.Release();
    }()).type, ErrorType) << "no such parent";

    const Frame badPreset = Execute(server, CommandType::CreateEntityEx, [&]
    {
        BufferWriter w;
        w.WriteString("X");
        w.WriteString("");
        w.WriteI32(Last);
        w.WriteString("Teapot");
        return w.Release();
    }());
    EXPECT_EQ(badPreset.type, ErrorType);
    EXPECT_NE(badPreset.Text().find("Teapot"), std::string::npos) << badPreset.Text();
    EXPECT_NE(badPreset.Text().find("Cube"), std::string::npos) << "it lists the presets: " << badPreset.Text();

    EXPECT_EQ(Nodes().size(), count);
    EXPECT_EQ(Revision(), revision);
}

// ==================== SetEntityParent ====================

TEST_F(EditorHierarchyTest, AnObjectMovesUnderAnotherAndBetweenSiblings)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    const std::string c = Create("C");
    const std::string x = Create("X", a);
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "X", "B", "C"}));

    ASSERT_EQ(Reparent(c, a, 0).type, OkType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "C", "X", "B"}));
    EXPECT_EQ(Node(c).at("parentId"), a);
    EXPECT_EQ(Node(c).at("index"), 0u);
    EXPECT_EQ(Node(x).at("index"), 1u);

    ASSERT_EQ(Reparent(c, NoParent, 1).type, OkType) << "to the roots, in second place";
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "X", "C", "B"}));
    EXPECT_EQ(Node(c).at("parentId"), "");
    EXPECT_EQ(Node(c).at("index"), 1u);

    ASSERT_EQ(Reparent(a, NoParent, Last).type, OkType) << "within the roots, to the end";
    EXPECT_EQ(Names(), (std::vector<std::string>{"C", "B", "A", "X"}));

    ASSERT_EQ(Reparent(b, a, 99).type, OkType) << "past the last is the last";
    EXPECT_EQ(Names(), (std::vector<std::string>{"C", "A", "X", "B"}));
    EXPECT_EQ(Node(b).at("index"), 1u);
}

TEST_F(EditorHierarchyTest, MovingAmongTheSiblingsOfOneParentReorders)
{
    const std::string parent = Create("P");
    const std::string one = Create("One", parent);
    const std::string two = Create("Two", parent);
    const std::string three = Create("Three", parent);

    ASSERT_EQ(Reparent(three, parent, 0).type, OkType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"P", "Three", "One", "Two"}));
    ASSERT_EQ(Reparent(three, parent, 1).type, OkType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"P", "One", "Three", "Two"}));
    ASSERT_EQ(Reparent(one, parent, Last).type, OkType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"P", "Three", "Two", "One"}));
    (void)two;
}

TEST_F(EditorHierarchyTest, MovingAnObjectToWhereItIsChangesNothing)
{
    const std::string parent = Create("P");
    const std::string one = Create("One", parent);
    const std::string two = Create("Two", parent);
    const uint32_t revision = Revision();

    EXPECT_EQ(Reparent(one, parent, 0).type, OkType);
    EXPECT_EQ(Reparent(two, parent, Last).type, OkType);
    EXPECT_EQ(Reparent(two, parent, 99).type, OkType);
    EXPECT_EQ(Reparent(parent, NoParent, 0).type, OkType);
    EXPECT_EQ(Revision(), revision) << "nothing moved, so the scene isn't made unsaved";
    EXPECT_EQ(SceneChangedEvents(server).back().at("revision"), revision) << "and no event says it did";
}

TEST_F(EditorHierarchyTest, TheWorldTransformIsKeptOrTheLocalOne)
{
    const std::string parent = Create("Parent");
    ASSERT_EQ(SetLocal(parent, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);
    const std::string kept = Create("Kept");
    const std::string moved = Create("Moved");
    ASSERT_EQ(SetLocal(kept, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);
    ASSERT_EQ(SetLocal(moved, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);

    ASSERT_EQ(Reparent(kept, parent, Last, true).type, OkType);
    const EntityData keptData = GetEntity(kept);
    EXPECT_NEAR(keptData.worldMatrix[12], 1.0f, 1e-4f) << "it stays where it was in the world";
    EXPECT_NEAR(keptData.entity.at("transform").at("position").at("x").get<float>(), -9.0f, 1e-4f)
        << "so its local position changed";

    ASSERT_EQ(Reparent(moved, parent, Last, false).type, OkType);
    const EntityData movedData = GetEntity(moved);
    EXPECT_NEAR(movedData.entity.at("transform").at("position").at("x").get<float>(), 1.0f, 1e-4f)
        << "it keeps its local position";
    EXPECT_NEAR(movedData.worldMatrix[12], 11.0f, 1e-4f) << "so it moved with the parent";
}

TEST_F(EditorHierarchyTest, AnObjectCantBecomeItsOwnDescendantsChild)
{
    const std::string a = Create("A");
    const std::string b = Create("B", a);
    const std::string c = Create("C", b);
    const uint32_t revision = Revision();
    const auto before = Names();

    EXPECT_EQ(Reparent(a, a, Last).type, ErrorType) << "under itself";
    const Frame cycle = Reparent(a, c, Last);
    EXPECT_EQ(cycle.type, ErrorType) << "under its own grandchild";
    EXPECT_NE(cycle.Text().find("descendant"), std::string::npos) << cycle.Text();
    EXPECT_EQ(Reparent(b, c, Last).type, ErrorType);

    EXPECT_EQ(Names(), before);
    EXPECT_EQ(Revision(), revision);
}

TEST_F(EditorHierarchyTest, AnUnknownObjectOrParentIsAnError)
{
    const std::string a = Create("A");
    const uint32_t revision = Revision();
    const std::string nobody = "00000000-0000-0000-0000-000000000000";

    EXPECT_EQ(Reparent(nobody, NoParent, Last).type, ErrorType);
    EXPECT_EQ(Reparent("not a uuid", NoParent, Last).type, ErrorType);
    EXPECT_EQ(Reparent(a, nobody, Last).type, ErrorType);
    EXPECT_EQ(Revision(), revision);
}

// ==================== SetEntityProperties ====================

TEST_F(EditorHierarchyTest, PropertiesChangeTheObject)
{
    const std::string id = Create("Before");
    const uint32_t revision = Revision();

    ASSERT_EQ(SetProperties(id, json{{"name", "After"}, {"tag", "Player"}, {"layer", 5}, {"active", false}}).type, OkType);
    const json node = Node(id);
    EXPECT_EQ(node.at("name"), "After");
    EXPECT_EQ(node.at("tag"), "Player");
    EXPECT_EQ(node.at("layer"), 5);
    EXPECT_EQ(node.at("active"), false);
    EXPECT_EQ(node.at("activeInHierarchy"), false);
    EXPECT_EQ(Revision(), revision + 1) << "one request is one change";

    ASSERT_EQ(SetProperties(id, json{{"active", true}}).type, OkType);
    EXPECT_EQ(Node(id).at("active"), true);
    EXPECT_EQ(Node(id).at("name"), "After") << "a key left out changes nothing";
}

TEST_F(EditorHierarchyTest, ValuesThatAreAlreadySetChangeNothing)
{
    const std::string id = Create("Same");
    const uint32_t revision = Revision();
    EXPECT_EQ(SetProperties(id, json{{"name", "Same"}, {"tag", "Untagged"}, {"layer", 0}, {"active", true}}).type, OkType);
    EXPECT_EQ(SetProperties(id, json::object()).type, OkType);
    EXPECT_EQ(Revision(), revision);
}

TEST_F(EditorHierarchyTest, ABadPropertyRefusesTheWholeRequest)
{
    const std::string id = Create("Keep");
    const uint32_t revision = Revision();

    // The valid keys come first in the object; none of them is applied
    for (const json &bad : {json{{"name", "Changed"}, {"layer", 32}},
                            json{{"name", "Changed"}, {"layer", -1}},
                            json{{"name", "Changed"}, {"layer", 1.5}},
                            json{{"name", "Changed"}, {"layer", "1"}},
                            json{{"name", "Changed"}, {"active", "yes"}},
                            json{{"name", "Changed"}, {"tag", 5}},
                            json{{"name", 5}},
                            json{{"name", "Changed"}, {"colour", 1}},
                            json::array(),
                            json("name")})
    {
        SCOPED_TRACE(bad.dump());
        EXPECT_EQ(SetProperties(id, bad).type, ErrorType);
        EXPECT_EQ(Node(id).at("name"), "Keep");
    }
    EXPECT_EQ(Revision(), revision);

    const Frame unknown = SetProperties(id, json{{"colour", 1}});
    EXPECT_NE(unknown.Text().find("colour"), std::string::npos) << unknown.Text();
    EXPECT_EQ(SetProperties("00000000-0000-0000-0000-000000000000", json{{"name", "X"}}).type, ErrorType);
}

TEST_F(EditorHierarchyTest, ChangingTheActiveStateRunsNoCallbacksOnASceneOpenedForEditing)
{
    ASSERT_TRUE(LoadedScene().IsEditMode());

    const std::string id = Create("Object");
    const auto object = LoadedScene().FindGameObjectByUUID(Math::UUID::FromString(id).value());
    ASSERT_NE(object, nullptr);
    object->AddComponent<HierarchyTestProbe>();
    HierarchyTestProbe::enableCalls = 0;
    HierarchyTestProbe::disableCalls = 0;

    // Off and on again, and under an inactive parent: a scene that runs would call OnDisable and OnEnable each time
    ASSERT_EQ(SetProperties(id, json{{"active", false}}).type, OkType);
    EXPECT_FALSE(object->IsActive());
    ASSERT_EQ(SetProperties(id, json{{"active", true}}).type, OkType);
    EXPECT_TRUE(object->IsActiveInHierarchy());

    const std::string off = Create("Off");
    ASSERT_EQ(SetProperties(off, json{{"active", false}}).type, OkType);
    ASSERT_EQ(Reparent(id, off, Last).type, OkType);
    EXPECT_FALSE(object->IsActiveInHierarchy());
    ASSERT_EQ(Reparent(id, NoParent, Last).type, OkType);
    EXPECT_TRUE(object->IsActiveInHierarchy());

    EXPECT_EQ(HierarchyTestProbe::enableCalls, 0);
    EXPECT_EQ(HierarchyTestProbe::disableCalls, 0);
}

// ==================== DuplicateEntity ====================

TEST_F(EditorHierarchyTest, ADuplicateIsNamedLikeUnityAndPlacedAfterTheOriginal)
{
    const std::string r = Create("R", NoParent, Last, "Cube");
    (void)Create("S");

    const std::string first = DuplicateId(r);
    EXPECT_NE(first, r);
    EXPECT_EQ(Names(), (std::vector<std::string>{"R", "R (1)", "S"}));
    EXPECT_EQ(Node(first).at("index"), 1u);

    (void)DuplicateId(r);
    EXPECT_EQ(Names(), (std::vector<std::string>{"R", "R (2)", "R (1)", "S"}));

    // A copy of a copy is a copy of the original name: the first free number
    (void)DuplicateId(first);
    const auto names = Names();
    EXPECT_EQ(names[3], "R (3)") << "right after R (1), which is at 2";
    EXPECT_EQ(names.size(), 5u);
}

TEST_F(EditorHierarchyTest, ADuplicateCopiesTheSubtreeWithFreshIds)
{
    const std::string root = Create("Root", NoParent, Last, "Cube");
    const std::string child = Create("Child", root, Last, "Sphere");
    const std::string grandchild = Create("Grandchild", child, Last, "PointLight");
    ASSERT_EQ(SetLocal(root, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {2.0f, 2.0f, 2.0f}).type, OkType);
    ASSERT_EQ(SetProperties(child, json{{"tag", "Enemy"}, {"layer", 3}, {"active", false}}).type, OkType);

    const std::string copy = DuplicateId(root);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Child", "Grandchild", "Root (1)", "Child", "Grandchild"}));

    const json nodes = Nodes();
    ASSERT_EQ(nodes.size(), 6u);
    EXPECT_EQ(nodes[3].at("id"), copy);
    EXPECT_EQ(nodes[4].at("parentId"), copy) << "the copy's child is under the copy";
    EXPECT_EQ(nodes[5].at("parentId"), nodes[4].at("id"));
    EXPECT_NE(nodes[4].at("id"), Node(child).at("id")) << "every object has a new UUID";
    EXPECT_NE(nodes[5].at("id"), Node(grandchild).at("id"));
    EXPECT_EQ(nodes[3].at("components"), (json::array({"CubeRenderer"})));
    EXPECT_EQ(nodes[4].at("components"), (json::array({"SphereRenderer"})));
    EXPECT_EQ(nodes[4].at("tag"), "Enemy");
    EXPECT_EQ(nodes[4].at("layer"), 3);
    EXPECT_EQ(nodes[4].at("active"), false);

    // The transform is copied too, and the components have their own UUIDs
    const EntityData original = GetEntity(root);
    const EntityData duplicate = GetEntity(copy);
    EXPECT_EQ(duplicate.entity.at("transform"), original.entity.at("transform"));
    EXPECT_NE(duplicate.entity.at("components")[0].at("uuid"), original.entity.at("components")[0].at("uuid"));

    // The original is as it was
    EXPECT_EQ(Node(root).at("name"), "Root");
    EXPECT_EQ(Node(child).at("parentId"), root);
}

TEST_F(EditorHierarchyTest, AChildsDuplicateStaysUnderItsParentRightAfterIt)
{
    const std::string parent = Create("P");
    const std::string a = Create("A", parent);
    (void)Create("B", parent);

    (void)DuplicateId(a);
    EXPECT_EQ(Names(), (std::vector<std::string>{"P", "A", "A (1)", "B"}));
    EXPECT_EQ(Node(DuplicateId(a)).at("parentId"), parent);
}

TEST_F(EditorHierarchyTest, ADuplicateKeepsReferencesToObjectsOutsideTheCopy)
{
    RegisterHolder();
    Scene &scene = LoadedScene();

    // Outsider is not part of what is copied; Holder points at it, and its child Inner points at Holder
    const auto outsider = GameObject::Create("Outsider");
    const auto holder = GameObject::Create("Holder");
    const auto inner = GameObject::Create("Inner");
    holder->AddChild(inner);
    holder->AddComponent<HierarchyTestHolder>()->target = outsider.get();
    inner->AddComponent<HierarchyTestHolder>()->target = holder.get();
    scene.AddRootGameObject(outsider);
    scene.AddRootGameObject(holder);

    const std::string copyId = DuplicateId(holder->GetUUID().ToString());
    const auto copy = scene.FindGameObjectByUUID(Math::UUID::FromString(copyId).value());
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->GetChildCount(), 1u);

    const auto *copiedHolder = copy->GetComponent<HierarchyTestHolder>();
    const auto *copiedInner = copy->GetChild(0)->GetComponent<HierarchyTestHolder>();
    ASSERT_NE(copiedHolder, nullptr);
    ASSERT_NE(copiedInner, nullptr);
    EXPECT_EQ(copiedHolder->target, outsider.get()) << "outside the copy: kept";
    EXPECT_EQ(copiedInner->target, copy.get()) << "inside the copy: the copy's";
    EXPECT_NE(copiedInner->target, holder.get());
    // The original still points where it did
    EXPECT_EQ(holder->GetComponent<HierarchyTestHolder>()->target, outsider.get());
    EXPECT_EQ(inner->GetComponent<HierarchyTestHolder>()->target, holder.get());
}

TEST_F(EditorHierarchyTest, DuplicatingAnUnknownObjectIsAnError)
{
    (void)Create("A");
    const uint32_t revision = Revision();
    EXPECT_EQ(Duplicate("00000000-0000-0000-0000-000000000000").type, ErrorType);
    EXPECT_EQ(Duplicate("nonsense").type, ErrorType);
    EXPECT_EQ(Revision(), revision);
}

// ==================== GetEntity ====================

TEST_F(EditorHierarchyTest, AnEntityComesWithItsHeaderTransformAndComponents)
{
    const std::string parent = Create("Parent");
    const std::string lamp = Create("Lamp", parent, Last, "PointLight");

    const EntityData data = GetEntity(lamp);
    const json &header = data.entity.at("header");
    EXPECT_EQ(header.at("id"), lamp);
    EXPECT_EQ(header.at("parentId"), parent);
    EXPECT_EQ(header.at("index"), 0u);
    EXPECT_EQ(header.at("name"), "Lamp");
    EXPECT_EQ(header.at("active"), true);
    EXPECT_EQ(header.at("activeInHierarchy"), true);
    EXPECT_EQ(header.at("layer"), 0);
    EXPECT_EQ(header.at("tag"), "Untagged");

    const json &transform = data.entity.at("transform");
    EXPECT_EQ(transform.at("position"), (json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}));
    EXPECT_EQ(transform.at("rotation"), (json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}));
    EXPECT_EQ(transform.at("scale"), (json{{"x", 1.0}, {"y", 1.0}, {"z", 1.0}}));

    const json &components = data.entity.at("components");
    ASSERT_EQ(components.size(), 1u);
    EXPECT_EQ(components[0].at("type"), "Light");
    EXPECT_TRUE(Math::UUID::FromString(components[0].at("uuid").get<std::string>()).has_value());
    // values is what the scene saves for the component
    const json &values = components[0].at("values");
    EXPECT_EQ(values.at("type"), "Point");
    EXPECT_TRUE(values.contains("intensity"));
    EXPECT_TRUE(values.contains("range"));
}

TEST_F(EditorHierarchyTest, TheWorldMatrixIsTheObjectsLocalToWorldMatrix)
{
    const std::string parent = Create("Parent");
    const std::string child = Create("Child", parent);
    ASSERT_EQ(SetLocal(parent, {10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {2.0f, 2.0f, 2.0f}).type, OkType);
    ASSERT_EQ(SetLocal(child, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);

    // Column-major: the translation is elements 12, 13 and 14; the child sits at the parent's position plus its own
    // offset scaled by the parent's scale
    const EntityData data = GetEntity(child);
    EXPECT_NEAR(data.worldMatrix[12], 12.0f, 1e-4f);
    EXPECT_NEAR(data.worldMatrix[13], 4.0f, 1e-4f);
    EXPECT_NEAR(data.worldMatrix[14], 6.0f, 1e-4f);
    EXPECT_NEAR(data.worldMatrix[15], 1.0f, 1e-4f);
    EXPECT_NEAR(data.worldMatrix[0], 2.0f, 1e-4f) << "x scale";
    EXPECT_NEAR(data.worldMatrix[5], 2.0f, 1e-4f) << "y scale";
    EXPECT_NEAR(data.worldMatrix[10], 2.0f, 1e-4f) << "z scale";
    EXPECT_NEAR(data.worldMatrix[3], 0.0f, 1e-4f);
    // Its local transform is its own
    EXPECT_NEAR(data.entity.at("transform").at("position").at("y").get<float>(), 2.0f, 1e-4f);
    EXPECT_NEAR(GetEntity(parent).worldMatrix[12], 10.0f, 1e-4f);
}

TEST_F(EditorHierarchyTest, AnObjectWithoutATransformHasNoneAndAnIdentityMatrix)
{
    // CreateEntity (the older command) makes an object with no transform
    const Frame created = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    ASSERT_EQ(created.type, EntityCreatedType);
    BufferReader r(created.payload);
    const std::string id = r.ReadString();

    const EntityData data = GetEntity(id);
    EXPECT_FALSE(data.entity.contains("transform"));
    EXPECT_EQ(data.entity.at("header").at("name"), "Bare");
    EXPECT_TRUE(data.entity.at("components").empty());
    for (size_t i = 0; i < 16; ++i)
    {
        EXPECT_FLOAT_EQ(data.worldMatrix[i], i % 5 == 0 ? 1.0f : 0.0f) << i;
    }
}

TEST_F(EditorHierarchyTest, AnUnknownEntityIsAnError)
{
    const Frame frame = Execute(server, CommandType::GetEntity, Strings({"00000000-0000-0000-0000-000000000000"}));
    EXPECT_EQ(frame.type, ErrorType);
    EXPECT_NE(frame.Text().find("not found"), std::string::npos) << frame.Text();
    EXPECT_EQ(Execute(server, CommandType::GetEntity, Strings({"nonsense"})).type, ErrorType);
}

// ==================== SetLocalTransform ====================

TEST_F(EditorHierarchyTest, SetLocalTransformSetsTheLocalTransform)
{
    const std::string id = Create("Thing");
    const uint32_t revision = Revision();

    ASSERT_EQ(SetLocal(id, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 1.0f, 1.0f}, {4.0f, 5.0f, 6.0f}).type, OkType);
    const json transform = GetEntity(id).entity.at("transform");
    EXPECT_NEAR(transform.at("position").at("x").get<float>(), 1.0f, 1e-5f);
    EXPECT_NEAR(transform.at("position").at("z").get<float>(), 3.0f, 1e-5f);
    EXPECT_NEAR(transform.at("scale").at("y").get<float>(), 5.0f, 1e-5f);
    // (0, 0, 1, 1) was normalised
    const float half = std::sqrt(0.5f);
    EXPECT_NEAR(transform.at("rotation").at("z").get<float>(), half, 1e-5f);
    EXPECT_NEAR(transform.at("rotation").at("w").get<float>(), half, 1e-5f);
    EXPECT_NEAR(transform.at("rotation").at("x").get<float>(), 0.0f, 1e-5f);
    EXPECT_EQ(Revision(), revision + 1);
}

TEST_F(EditorHierarchyTest, SetLocalTransformWithTheValuesItHasChangesNothing)
{
    const std::string id = Create("Thing");
    ASSERT_EQ(SetLocal(id, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 2.0f}, {2.0f, 2.0f, 2.0f}).type, OkType);
    const uint32_t revision = Revision();
    const size_t events = SceneChangedEvents(server).size();

    // The same values (the rotation normalises to the stored one)
    EXPECT_EQ(SetLocal(id, {1.0f, 2.0f, 3.0f}, {0.0f, 0.0f, 0.0f, 5.0f}, {2.0f, 2.0f, 2.0f}).type, OkType);
    EXPECT_EQ(Revision(), revision);
    EXPECT_EQ(SceneChangedEvents(server).size(), events);

    EXPECT_EQ(SetLocal(id, {1.0f, 2.0f, 4.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {2.0f, 2.0f, 2.0f}).type, OkType);
    EXPECT_EQ(Revision(), revision + 1);
}

TEST_F(EditorHierarchyTest, SetLocalTransformGivesAnObjectWithNoTransformOne)
{
    const Frame created = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    BufferReader r(created.payload);
    const std::string id = r.ReadString();
    ASSERT_FALSE(GetEntity(id).entity.contains("transform"));

    ASSERT_EQ(SetLocal(id, {7.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);
    const EntityData data = GetEntity(id);
    ASSERT_TRUE(data.entity.contains("transform"));
    EXPECT_NEAR(data.worldMatrix[12], 7.0f, 1e-5f);
}

TEST_F(EditorHierarchyTest, SetLocalTransformRefusesWhatIsntATransform)
{
    const std::string id = Create("Thing");
    const uint32_t revision = Revision();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    EXPECT_EQ(SetLocal(id, {nan, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, ErrorType) << "NaN position";
    EXPECT_EQ(SetLocal(id, {0.0f, inf, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, ErrorType) << "infinite position";
    EXPECT_EQ(SetLocal(id, {0.0f, 0.0f, 0.0f}, {0.0f, nan, 0.0f, 1.0f}).type, ErrorType) << "NaN rotation";
    EXPECT_EQ(SetLocal(id, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, nan, 1.0f}).type, ErrorType)
        << "NaN scale";
    const Frame zero = SetLocal(id, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f});
    EXPECT_EQ(zero.type, ErrorType) << "a rotation with no direction";
    EXPECT_NE(zero.Text().find("zero"), std::string::npos) << zero.Text();
    EXPECT_EQ(SetLocal(id, {0.0f, 0.0f, 0.0f}, {1e38f, 0.0f, 0.0f, 0.0f}).type, ErrorType)
        << "a rotation whose length overflows";
    EXPECT_EQ(SetLocal("00000000-0000-0000-0000-000000000000", {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type,
              ErrorType);

    EXPECT_EQ(Revision(), revision);
    EXPECT_NEAR(GetEntity(id).worldMatrix[12], 0.0f, 1e-6f);
}

// ==================== The revision and sceneChanged ====================

TEST_F(EditorHierarchyTest, EveryHierarchyCommandMovesTheRevisionOnce)
{
    uint32_t revision = Revision();
    const auto movedOnce = [&](const char *what)
    {
        const uint32_t now = Revision();
        EXPECT_EQ(now, revision + 1) << what;
        revision = now;
    };

    const std::string a = Create("A");
    movedOnce("CreateEntityEx");
    const std::string b = Create("B");
    movedOnce("CreateEntityEx");
    ASSERT_EQ(Reparent(b, a, Last).type, OkType);
    movedOnce("SetEntityParent");
    ASSERT_EQ(SetProperties(a, json{{"name", "Renamed"}}).type, OkType);
    movedOnce("SetEntityProperties");
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);
    movedOnce("SetLocalTransform");
    (void)DuplicateId(a);
    movedOnce("DuplicateEntity");
    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({a})).type, OkType);
    movedOnce("DestroyEntity");
}

TEST_F(EditorHierarchyTest, SceneChangedSaysWhichObjectsChanged)
{
    const std::string a = Create("A");
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({a})));
    EXPECT_FALSE(SceneChangedEvents(server).back().contains("full"));

    const std::string b = Create("B");
    const std::string c = Create("C", b);

    ASSERT_EQ(Reparent(b, a, Last).type, OkType);
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({b, c})))
        << "the object and its subtree: their world transforms moved with the parent";

    ASSERT_EQ(SetProperties(a, json{{"tag", "Hero"}}).type, OkType);
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({a})));

    ASSERT_EQ(SetLocal(c, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({c})));
    ASSERT_EQ(SetLocal(b, {2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType);
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({b, c})))
        << "a child's world matrix follows its parent";
    ASSERT_EQ(SetProperties(b, json{{"active", false}}).type, OkType);
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({b, c})))
        << "activeInHierarchy reaches the subtree";

    // A duplicate and its whole subtree are new; a destroyed subtree is gone
    const std::string copy = DuplicateId(a);
    const json created = SceneChangedEvents(server).back().at("entityIds");
    ASSERT_EQ(created.size(), 3u) << "the copy, its child and its grandchild";
    EXPECT_EQ(created[0], copy);

    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({a})).type, OkType);
    const json destroyed = SceneChangedEvents(server).back().at("entityIds");
    ASSERT_EQ(destroyed.size(), 3u) << "A, B and C";
    EXPECT_EQ(destroyed[0], a);

    // The events carry the revision they were pushed at
    const auto events = SceneChangedEvents(server);
    EXPECT_EQ(events.back().at("revision"), Revision());
}

TEST_F(EditorHierarchyTest, LoadingAnotherSceneIsFull)
{
    (void)Create("A");

    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Another"})).type, SceneInfoType);
    json event = SceneChangedEvents(server).back();
    EXPECT_EQ(event.at("full"), true);
    EXPECT_FALSE(event.contains("entityIds"));

    ASSERT_EQ(Execute(server, CommandType::LoadScene, Strings({R"({"name":"Sent","rootGameObjects":[]})"})).type, OkType);
    event = SceneChangedEvents(server).back();
    EXPECT_EQ(event.at("full"), true);
    EXPECT_FALSE(event.contains("entityIds"));
    EXPECT_EQ(event.at("revision"), Revision());
}

TEST_F(EditorHierarchyTest, AChangeThatTouchedTooManyObjectsCarriesNoIds)
{
    // 300 children under one object, made directly: deleting it touches more objects than an event lists
    Scene &scene = LoadedScene();
    const auto big = GameObject::Create("Big");
    scene.AddRootGameObject(big);
    for (size_t i = 0; i < EditorServer::MaxEventEntityIds + 44; ++i)
    {
        big->AddChild(GameObject::Create("Piece"));
    }
    const uint32_t revision = Revision();

    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({big->GetUUID().ToString()})).type, OkType);
    const json event = SceneChangedEvents(server).back();
    EXPECT_FALSE(event.contains("entityIds")) << "more than " << EditorServer::MaxEventEntityIds;
    EXPECT_FALSE(event.contains("full"));
    EXPECT_GT(event.at("revision").get<uint32_t>(), revision) << "a client still sees that something changed";
    EXPECT_TRUE(Nodes().empty());
}

TEST_F(EditorHierarchyTest, TheOlderEntityCommandsSayWhichObjectToo)
{
    const Frame created = Execute(server, CommandType::CreateEntity, Strings({"Old"}));
    BufferReader r(created.payload);
    const std::string id = r.ReadString();
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({id})));

    ASSERT_EQ(SetLocal(id, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}).type, OkType); // gives it a transform
    BufferWriter transform;
    transform.WriteString(id);
    for (int i = 0; i < 9; ++i)
        transform.WriteF32(1.0f);
    ASSERT_EQ(Execute(server, CommandType::SetEntityTransform, transform.Release()).type, OkType);
    EXPECT_EQ(SceneChangedEvents(server).back().at("entityIds"), (json::array({id})));
}

// ==================== Edit mode ====================

TEST_F(EditorHierarchyTest, EverySceneTheHostLoadsIsOpenedForEditing)
{
    EXPECT_TRUE(LoadedScene().IsEditMode()) << "NewScene";

    ASSERT_EQ(Execute(server, CommandType::LoadScene, Strings({R"({"name":"Sent","rootGameObjects":[]})"})).type, OkType);
    ASSERT_EQ(LoadedScene().sceneName, "Sent");
    EXPECT_TRUE(LoadedScene().IsEditMode()) << "LoadScene";
}
