#include <gtest/gtest.h>

#include <cstdint>
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
#include <editor-server/TokenTable.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;

// SerializeEntities and InstantiateEntities (protocol 1.18.0, EN2) through EditorServer::ExecuteCommand, on scenes made
// with NewScene. Part of the project tests program: these tests load scenes into SceneManager.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t HierarchyType = static_cast<uint8_t>(ResponseType::Hierarchy);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t SerializedType = static_cast<uint8_t>(ResponseType::SerializedEntities);
    constexpr uint8_t CreatedType = static_cast<uint8_t>(ResponseType::EntitiesCreated);

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

    /// A component with references to a GameObject and to a Component, to see what a paste does with them
    class ClipboardTestHolder final : public SerializableComponent
    {
    public:
        explicit ClipboardTestHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterGameObjectRef("target", target);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "EditorClipboardTest_Holder"; }

        GameObject *target = nullptr;
    };

    void RegisterHolder()
    {
        ComponentRegistry::Instance().Register(
            "EditorClipboardTest_Holder",
            [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<ClipboardTestHolder>(gameObject); });
    }

    /// The text of a payload with objects named `names`, each a bare object (for the limit and malformed tests)
    std::string PayloadText(const json &entities, const std::string &format = "n2engine.entities", int version = 1)
    {
        return json{{"format", format}, {"version", version}, {"objectCount", 0}, {"entities", entities}}.dump();
    }

    /// A single tree of `levels` levels, each object with one child
    std::string ChainText(const std::size_t levels)
    {
        std::string text = R"({"format":"n2engine.entities","version":1,"entities":[)";
        for (std::size_t i = 0; i < levels; ++i)
            text += R"({"name":"n","uuid":")" + Math::UUID::Random().ToString() + R"(","children":[)";
        for (std::size_t i = 0; i < levels; ++i)
            text += "]}";
        text += "]}";
        return text;
    }

    class EditorClipboardTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            RegisterHolder();
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Clipboard Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
        }

        std::string Create(const std::string &name, const std::string &parentId = "", int32_t siblingIndex = Last,
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

        Frame SerializeFrame(const json &ids)
        {
            BufferWriter w;
            w.WriteString(ids.dump());
            return Execute(server, CommandType::SerializeEntities, w.Release());
        }

        /// The ClipboardPayload SerializeEntities answers for these ids
        json Serialize(const std::vector<std::string> &ids)
        {
            const Frame frame = SerializeFrame(json(ids));
            EXPECT_EQ(frame.type, SerializedType) << frame.Text();
            if (frame.type != SerializedType)
                return json::object();
            BufferReader r(frame.payload);
            const json clipboard = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return clipboard;
        }

        Frame InstantiateFrame(const std::string &text, const std::string &parentId = "", int32_t siblingIndex = Last,
                               bool keepExternalRefs = false)
        {
            BufferWriter w;
            w.WriteString(text);
            w.WriteString(parentId);
            w.WriteI32(siblingIndex);
            w.WriteBool(keepExternalRefs);
            return Execute(server, CommandType::InstantiateEntities, w.Release());
        }

        /// The ids InstantiateEntities answers
        std::vector<std::string> Instantiate(const json &clipboard, const std::string &parentId = "",
                                             int32_t siblingIndex = Last, bool keepExternalRefs = false)
        {
            const Frame frame = InstantiateFrame(clipboard.dump(), parentId, siblingIndex, keepExternalRefs);
            EXPECT_EQ(frame.type, CreatedType) << frame.Text();
            if (frame.type != CreatedType)
                return {};
            BufferReader r(frame.payload);
            const json ids = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return ids.get<std::vector<std::string>>();
        }

        json Nodes()
        {
            const Frame frame = Execute(server, CommandType::GetHierarchy);
            EXPECT_EQ(frame.type, HierarchyType) << frame.Text();
            if (frame.type != HierarchyType)
                return json::array();
            BufferReader r(frame.payload);
            (void)r.ReadU32();
            return ReadJson(r);
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

        json GetEntity(const std::string &id)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({id}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
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

        /// Undo or Redo: the step's label
        std::string Step(CommandType type)
        {
            const Frame frame = Execute(server, type);
            EXPECT_EQ(frame.type, EditResultType) << frame.Text();
            if (frame.type != EditResultType)
                return {};
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        [[nodiscard]] static Scene &LoadedScene() { return SceneManager::GetCurSceneRef(); }

        EditorServer server;
    };
}

// ==================== SerializeEntities ====================

TEST_F(EditorClipboardTest, ASelectionSerializesAsASelfContainedPayloadOfItsTopmostSubtrees)
{
    const std::string root = Create("Root", "", Last, "Cube");
    const std::string child = Create("Child", root, Last, "Sphere");
    const std::string grandchild = Create("Grandchild", child);
    const std::string other = Create("Other");
    (void)Create("Unselected");

    // The descendants are covered by the ancestor; the order is the scene's, whatever the order asked for
    const json payload = Serialize({other, grandchild, root, child, root});
    EXPECT_EQ(payload.at("format"), "n2engine.entities");
    EXPECT_EQ(payload.at("version"), 1);
    EXPECT_EQ(payload.at("objectCount"), 4);
    ASSERT_EQ(payload.at("entities").size(), 2u);
    EXPECT_EQ(payload.at("entities")[0].at("name"), "Root");
    EXPECT_EQ(payload.at("entities")[1].at("name"), "Other");
    ASSERT_EQ(payload.at("entities")[0].at("children").size(), 1u);
    EXPECT_EQ(payload.at("entities")[0].at("children")[0].at("children")[0].at("name"), "Grandchild");
    EXPECT_FALSE(payload.dump().find("Unselected") != std::string::npos);
}

TEST_F(EditorClipboardTest, SerializingChangesNothing)
{
    const std::string a = Create("A");
    const uint32_t revision = Revision();
    (void)Serialize({a});
    EXPECT_EQ(Revision(), revision);
    EXPECT_NE(Step(CommandType::Undo), "") << "the creation is still the step to undo";
    EXPECT_TRUE(Names().empty());
}

TEST_F(EditorClipboardTest, SerializeRefusesWhatItCannotCopy)
{
    const std::string a = Create("A");
    const uint32_t revision = Revision();
    EXPECT_EQ(SerializeFrame(json::array()).type, ErrorType) << "nothing selected";
    EXPECT_EQ(SerializeFrame(json::object()).type, ErrorType) << "not an array";
    EXPECT_EQ(SerializeFrame(json::array({5})).type, ErrorType) << "not an id";
    EXPECT_EQ(SerializeFrame(json::array({"nonsense"})).type, ErrorType);
    EXPECT_EQ(SerializeFrame(json::array({a, "00000000-0000-0000-0000-000000000000"})).type, ErrorType)
        << "one unknown id refuses the whole copy";

    json tooMany = json::array();
    for (int i = 0; i < 4097; ++i)
        tooMany.push_back(a);
    EXPECT_EQ(SerializeFrame(tooMany).type, ErrorType);

    BufferWriter notJson;
    notJson.WriteString("[not json");
    EXPECT_EQ(Execute(server, CommandType::SerializeEntities, notJson.Release()).type, ErrorType);
    EXPECT_EQ(Revision(), revision);
}

TEST_F(EditorClipboardTest, ASelectionOfMoreThanTheObjectLimitIsRefused)
{
    const std::string root = Create("Root");
    Scene &scene = LoadedScene();
    const auto rootObject = scene.FindGameObjectByUUID(Math::UUID::FromString(root).value());
    ASSERT_NE(rootObject, nullptr);
    for (int i = 0; i < 10000; ++i)
        rootObject->AddChild(GameObject::Create("c"));

    const Frame frame = SerializeFrame(json::array({root}));
    EXPECT_EQ(frame.type, ErrorType);
    EXPECT_NE(frame.Text().find("10000"), std::string::npos) << frame.Text();
}

// ==================== InstantiateEntities ====================

TEST_F(EditorClipboardTest, ARoundTripBuildsTheSubtreeWithFreshIdsAndUniqueNames)
{
    const std::string root = Create("Root", "", Last, "Cube");
    const std::string child = Create("Child", root, Last, "Sphere");
    const std::string grandchild = Create("Grandchild", child, Last, "PointLight");

    BufferWriter local;
    local.WriteString(root);
    for (const float v : {1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 2.0f})
        local.WriteF32(v);
    ASSERT_EQ(Execute(server, CommandType::SetLocalTransform, local.Release()).type, OkType);
    BufferWriter props;
    props.WriteString(child);
    WriteJson(props, json{{"tag", "Enemy"}, {"layer", 3}, {"active", false}});
    ASSERT_EQ(Execute(server, CommandType::SetEntityProperties, props.Release()).type, OkType);

    const json payload = Serialize({root});
    const std::vector<std::string> created = Instantiate(payload);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_NE(created[0], root);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Root", "Child", "Grandchild", "Root (1)", "Child", "Grandchild"}));

    const json nodes = Nodes();
    ASSERT_EQ(nodes.size(), 6u);
    EXPECT_EQ(nodes[3].at("id"), created[0]);
    EXPECT_EQ(nodes[3].at("parentId"), "");
    EXPECT_EQ(nodes[4].at("parentId"), created[0]);
    EXPECT_EQ(nodes[5].at("parentId"), nodes[4].at("id"));
    EXPECT_NE(nodes[4].at("id"), child) << "every object has a new UUID";
    EXPECT_NE(nodes[5].at("id"), grandchild);
    EXPECT_EQ(nodes[4].at("tag"), "Enemy");
    EXPECT_EQ(nodes[4].at("layer"), 3);
    EXPECT_EQ(nodes[4].at("active"), false);
    EXPECT_EQ(nodes[3].at("components"), (json::array({"CubeRenderer"})));
    EXPECT_EQ(nodes[4].at("components"), (json::array({"SphereRenderer"})));

    const json original = GetEntity(root);
    const json pasted = GetEntity(created[0]);
    EXPECT_EQ(pasted.at("transform"), original.at("transform"));
    EXPECT_NE(pasted.at("components")[0].at("uuid"), original.at("components")[0].at("uuid"));

    // The same payload pasted again is numbered on, and the original is untouched
    (void)Instantiate(payload);
    EXPECT_EQ(Names().back(), "Grandchild");
    EXPECT_EQ(Names()[6], "Root (2)");
    EXPECT_EQ(Node(root).at("name"), "Root");
}

TEST_F(EditorClipboardTest, SeveralRootsPasteAsOneUndoStepInOrder)
{
    const std::string a = Create("A");
    const std::string b = Create("B", a);
    const std::string c = Create("C");
    const json payload = Serialize({a, c});
    ASSERT_EQ(payload.at("entities").size(), 2u);
    const size_t before = Names().size();

    const std::vector<std::string> created = Instantiate(payload);
    ASSERT_EQ(created.size(), 2u);
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "B", "C", "A (1)", "B", "C (1)"}));
    EXPECT_EQ(Node(created[0]).at("index"), 2u);
    EXPECT_EQ(Node(created[1]).at("index"), 3u);

    EXPECT_EQ(Step(CommandType::Undo), "Paste 2 objects") << "one step for the whole paste";
    EXPECT_EQ(Names().size(), before);
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "B", "C"}));
    EXPECT_EQ(Step(CommandType::Redo), "Paste 2 objects");
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "B", "C", "A (1)", "B", "C (1)"}));
    EXPECT_EQ(Node(created[0]).at("index"), 2u) << "redo puts them back with the same ids and places";
    EXPECT_EQ(Node(created[1]).at("index"), 3u);

    // And undone again, the step before is the creation of C
    EXPECT_EQ(Step(CommandType::Undo), "Paste 2 objects");
    EXPECT_EQ(Step(CommandType::Undo), "Create C");
    (void)b;
}

TEST_F(EditorClipboardTest, AOneObjectPasteIsLabelledWithItsName)
{
    const std::string a = Create("Thing");
    (void)Instantiate(Serialize({a}));
    EXPECT_EQ(Step(CommandType::Undo), "Paste Thing (1)");
}

TEST_F(EditorClipboardTest, ThePasteGoesUnderTheParentAtTheSiblingIndex)
{
    const std::string parent = Create("P");
    (void)Create("X", parent);
    (void)Create("Y", parent);
    const std::string source = Create("Src");
    const json payload = Serialize({source});

    const std::vector<std::string> created = Instantiate(payload, parent, 1);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(Node(created[0]).at("parentId"), parent);
    EXPECT_EQ(Node(created[0]).at("index"), 1u);
    EXPECT_EQ(Node(created[0]).at("name"), "Src") << "no sibling has the name: kept";

    const std::vector<std::string> past = Instantiate(payload, parent, 99);
    EXPECT_EQ(Node(past[0]).at("index"), 3u) << "past the last place: the last place";
    EXPECT_EQ(Node(past[0]).at("name"), "Src (1)");

    EXPECT_EQ(InstantiateFrame(payload.dump(), "00000000-0000-0000-0000-000000000000").type, ErrorType);
    EXPECT_EQ(InstantiateFrame(payload.dump(), "nonsense").type, ErrorType);
}

TEST_F(EditorClipboardTest, ReferencesInsideThePastedSetPointAtTheCopiesAcrossRoots)
{
    Scene &scene = LoadedScene();
    const auto first = GameObject::Create("First");
    const auto second = GameObject::Create("Second");
    const auto inner = GameObject::Create("Inner");
    second->AddChild(inner);
    first->AddComponent<ClipboardTestHolder>()->target = inner.get(); // into the other root's tree
    inner->AddComponent<ClipboardTestHolder>()->target = first.get();
    scene.AddRootGameObject(first);
    scene.AddRootGameObject(second);

    const json payload = Serialize({first->GetUUID().ToString(), second->GetUUID().ToString()});
    const std::vector<std::string> created = Instantiate(payload);
    ASSERT_EQ(created.size(), 2u);
    const auto copyFirst = scene.FindGameObjectByUUID(Math::UUID::FromString(created[0]).value());
    const auto copySecond = scene.FindGameObjectByUUID(Math::UUID::FromString(created[1]).value());
    ASSERT_NE(copyFirst, nullptr);
    ASSERT_NE(copySecond, nullptr);
    ASSERT_EQ(copySecond->GetChildCount(), 1u);

    const auto *firstHolder = copyFirst->GetComponent<ClipboardTestHolder>();
    const auto *innerHolder = copySecond->GetChild(0)->GetComponent<ClipboardTestHolder>();
    ASSERT_NE(firstHolder, nullptr);
    ASSERT_NE(innerHolder, nullptr);
    EXPECT_EQ(firstHolder->target, copySecond->GetChild(0).get());
    EXPECT_EQ(innerHolder->target, copyFirst.get());
    // The originals still point at each other
    EXPECT_EQ(first->GetComponent<ClipboardTestHolder>()->target, inner.get());
}

TEST_F(EditorClipboardTest, AReferenceOutsideThePastedSetIsKeptOnlyWhenAskedAndPresent)
{
    Scene &scene = LoadedScene();
    const auto outsider = GameObject::Create("Outsider");
    const auto holder = GameObject::Create("Holder");
    holder->AddComponent<ClipboardTestHolder>()->target = outsider.get();
    scene.AddRootGameObject(outsider);
    scene.AddRootGameObject(holder);
    const json payload = Serialize({holder->GetUUID().ToString()});

    const auto pasted = [&](bool keep)
    {
        const std::vector<std::string> created = Instantiate(payload, "", Last, keep);
        EXPECT_EQ(created.size(), 1u);
        const auto copy = scene.FindGameObjectByUUID(Math::UUID::FromString(created.at(0)).value());
        EXPECT_NE(copy, nullptr);
        return copy->GetComponent<ClipboardTestHolder>()->target;
    };
    EXPECT_EQ(pasted(true), outsider.get());
    EXPECT_EQ(pasted(false), nullptr);

    // Gone from the scene: nothing to keep
    BufferWriter destroy;
    destroy.WriteString(outsider->GetUUID().ToString());
    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, destroy.Release()).type, OkType);
    EXPECT_EQ(pasted(true), nullptr);
}

TEST_F(EditorClipboardTest, APayloadFromAnotherSceneKeepsItsNamesAndLosesItsExternalReferences)
{
    Scene &scene = LoadedScene();
    const auto outsider = GameObject::Create("Outsider");
    const auto holder = GameObject::Create("Holder");
    holder->AddChild(GameObject::Create("Kid"));
    holder->AddComponent<ClipboardTestHolder>()->target = outsider.get();
    scene.AddRootGameObject(outsider);
    scene.AddRootGameObject(holder);
    const json payload = Serialize({holder->GetUUID().ToString()});

    // A different scene: the payload is only text
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Other Scene"})).type, SceneInfoType);
    EXPECT_TRUE(Names().empty());
    const std::vector<std::string> created = Instantiate(payload, "", Last, true);
    ASSERT_EQ(created.size(), 1u);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Holder", "Kid"})) << "nothing to collide with";
    const auto copy = LoadedScene().FindGameObjectByUUID(Math::UUID::FromString(created[0]).value());
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->GetComponent<ClipboardTestHolder>()->target, nullptr) << "the target is not in this scene";
}

TEST_F(EditorClipboardTest, ThePasteIsRefusedInPlaceOfBuildingWhatExceedsALimit)
{
    (void)Create("Anchor");
    const uint32_t revision = Revision();
    const auto refused = [&](const std::string &text, const char *why)
    {
        const Frame frame = InstantiateFrame(text);
        EXPECT_EQ(frame.type, ErrorType) << why;
        EXPECT_EQ(Revision(), revision) << why << ": nothing changed";
        EXPECT_EQ(Names(), (std::vector<std::string>{"Anchor"})) << why;
        return frame.Text();
    };

    // Too many objects
    json many = json::array();
    json root = {{"name", "R"}, {"children", json::array()}};
    for (int i = 0; i < 10000; ++i)
        root["children"].push_back(json{{"name", "c"}});
    many.push_back(root);
    EXPECT_NE(refused(PayloadText(many), "10001 objects").find("10000"), std::string::npos);

    // Too many roots
    json roots = json::array();
    for (int i = 0; i < 1025; ++i)
        roots.push_back(json{{"name", "r"}});
    (void)refused(PayloadText(roots), "1025 roots");

    // Too deep: 513 levels is under the text nesting cap but over the hierarchy cap
    (void)refused(ChainText(513), "513 levels");
    // Far too deep for the text: refused before it is parsed
    (void)refused(ChainText(5000), "5000 levels");

    // Too large
    (void)refused(std::string(16u * 1024u * 1024u + 1, ' '), "over 16 MiB");

    // The deepest allowed tree is accepted, and below a parent it counts the parent's levels too
    const Frame fits = InstantiateFrame(ChainText(512));
    EXPECT_EQ(fits.type, CreatedType) << fits.Text();
    ASSERT_EQ(Step(CommandType::Undo).substr(0, 5), "Paste");
    const std::string parent = Create("P");
    EXPECT_EQ(InstantiateFrame(ChainText(512), parent).type, ErrorType) << "P is one more level";
    EXPECT_EQ(InstantiateFrame(ChainText(511), parent).type, CreatedType);
}

TEST_F(EditorClipboardTest, MalformedPayloadsAreRefusedWithoutChangingTheSceneOrEchoingTheText)
{
    (void)Create("Anchor");
    const uint32_t revision = Revision();
    const auto refused = [&](const std::string &text)
    {
        const Frame frame = InstantiateFrame(text);
        EXPECT_EQ(frame.type, ErrorType) << text.substr(0, 80);
        EXPECT_EQ(Revision(), revision) << text.substr(0, 80);
        EXPECT_EQ(frame.Text().find("SECRETMARK"), std::string::npos) << "the message echoes the payload: " << frame.Text();
        return frame.Text();
    };

    refused("");
    refused("not json SECRETMARK");
    refused("[1,2,3]");
    refused("42");
    refused(PayloadText(json::array({json{{"name", "x"}}}), "SECRETMARK"));
    refused(PayloadText(json::array({json{{"name", "x"}}}), "n2engine.entities", 2));
    refused(PayloadText(json::array({json{{"name", "x"}}}), "n2engine.entities", 0));
    refused(R"({"format":"n2engine.entities","version":"1","entities":[{"name":"x"}]})");
    refused(R"({"format":"n2engine.entities","version":1})");
    refused(R"({"format":"n2engine.entities","version":1,"entities":{"name":"x"}})");
    refused(PayloadText(json::array()));
    refused(PayloadText(json::array({"SECRETMARK"})));
    refused(PayloadText(json::array({json{{"name", "x"}}, 5})));

    // Trailing bytes after the request fields
    BufferWriter extra;
    extra.WriteString(PayloadText(json::array({json{{"name", "x"}}})));
    extra.WriteString("");
    extra.WriteI32(-1);
    extra.WriteBool(false);
    extra.WriteU8(0);
    EXPECT_EQ(Execute(server, CommandType::InstantiateEntities, extra.Release()).type, ErrorType);
    EXPECT_EQ(Revision(), revision);

    // A short request is a malformed payload, answered, not a crash
    BufferWriter shortRequest;
    shortRequest.WriteString("{}");
    EXPECT_EQ(Execute(server, CommandType::InstantiateEntities, shortRequest.Release()).type, ErrorType);
    EXPECT_EQ(Revision(), revision);
}

TEST_F(EditorClipboardTest, AssetPathsThatLeaveTheProjectAreRefusedWithTheWholePayload)
{
    (void)Create("Anchor");
    const uint32_t revision = Revision();
    for (const char *path : {"res://../outside.png", "res://textures/../../x", "res://a\\..\\b", "res://C:/x",
                             "res://a/./b", "res://nul", "res:///abs"})
    {
        json object = {{"name", "x"}, {"components", json::array({json{{"type", "CubeRenderer"}, {"data", json{{"texture", path}}}}})}};
        const Frame frame = InstantiateFrame(PayloadText(json::array({object})));
        EXPECT_EQ(frame.type, ErrorType) << path;
        EXPECT_EQ(frame.Text().find(path), std::string::npos) << "the path is not echoed: " << frame.Text();
        EXPECT_EQ(Revision(), revision) << path;
    }
    // Paths inside the project are not this check's business; the paste reads no file
    json fine = {{"name", "res://textures/wall.png"}, {"uuid", Math::UUID::Random().ToString()}};
    EXPECT_EQ(InstantiateFrame(PayloadText(json::array({fine}))).type, CreatedType);
}

TEST_F(EditorClipboardTest, ObjectsCopiedWithAGroupJoinTheGroupsStep)
{
    const std::string a = Create("A");
    const json payload = Serialize({a});
    ASSERT_EQ(Execute(server, CommandType::BeginEditGroup, Strings({"Two pastes"})).type, OkType);
    (void)Instantiate(payload);
    (void)Instantiate(payload);
    ASSERT_EQ(Execute(server, CommandType::EndEditGroup).type, OkType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"A", "A (1)", "A (2)"}));
    EXPECT_EQ(Step(CommandType::Undo), "Two pastes");
    EXPECT_EQ(Names(), (std::vector<std::string>{"A"}));
}

TEST_F(EditorClipboardTest, ViewersMayCopyButNotPaste)
{
    const auto id = [](CommandType type) { return static_cast<uint8_t>(type); };
    for (const TokenRole role : {TokenRole::Viewer, TokenRole::Editor, TokenRole::Admin})
        EXPECT_TRUE(EditorServer::RoleAllows(role, id(CommandType::SerializeEntities))) << TokenRoleName(role);
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id(CommandType::InstantiateEntities)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id(CommandType::InstantiateEntities)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id(CommandType::InstantiateEntities)));
}

TEST_F(EditorClipboardTest, AReadOnlyHostStillCopiesButRefusesToPaste)
{
    const std::string a = Create("A");
    const json payload = Serialize({a});
    ASSERT_TRUE(server.SetReadOnly(true));
    EXPECT_EQ(SerializeFrame(json::array({a})).type, SerializedType);
    EXPECT_EQ(InstantiateFrame(payload.dump()).type, ErrorType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"A"}));
}
