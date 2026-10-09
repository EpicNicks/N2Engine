#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditHistory.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// Undo, redo, edit groups, coalescing and autosave (#78, E6) through EditorServer::ExecuteCommand, on scenes made with
// NewScene or opened from a project. With the other project tests, a program of its own: these tests load scenes into
// SceneManager, which the other editor-server tests expect to be empty.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t ComponentAddedType = static_cast<uint8_t>(ResponseType::ComponentAdded);
    constexpr uint8_t ComponentDataType = static_cast<uint8_t>(ResponseType::ComponentData);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t HistoryType = static_cast<uint8_t>(ResponseType::History);
    constexpr uint8_t AutosaveType = static_cast<uint8_t>(ResponseType::Autosave);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);

    constexpr const char *HolderType = "EditorHistoryTest_Holder";

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

    /// A component with a number (clamped), a string, and references to an object and to another of its kind
    class HistoryHolder final : public SerializableComponent
    {
    public:
        explicit HistoryHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("count", count).Range(0.0, 10.0);
            RegisterMember("label", label);
            RegisterGameObjectRef("target", target);
            RegisterComponentRef("peer", peer);
        }

        [[nodiscard]] std::string GetTypeName() const override { return HolderType; }

        int count = 1;
        std::string label = "a";
        GameObject *target = nullptr;
        HistoryHolder *peer = nullptr;
        int activeChanges = 0;

    protected:
        void OnActiveFlagChanged() override { ++activeChanges; }
    };

    void RegisterHolder()
    {
        ComponentRegistry::Instance().Register(
            HolderType,
            [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<HistoryHolder>(gameObject); });
    }

    struct EditResultData
    {
        std::string label;
        uint32_t revision = 0;
        bool canUndo = false;
        bool canRedo = false;
        uint32_t savedRevision = 0;
    };

    EditResultData DecodeEditResult(const Frame &frame)
    {
        EXPECT_EQ(frame.type, EditResultType) << frame.Text();
        EditResultData data;
        if (frame.type != EditResultType)
            return data;
        BufferReader r(frame.payload);
        data.label = r.ReadString();
        data.revision = r.ReadU32();
        data.canUndo = r.ReadBool();
        data.canRedo = r.ReadBool();
        data.savedRevision = r.ReadU32();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after EditResult";
        return data;
    }

    struct SceneInfoData
    {
        std::string path;
        std::string name;
        std::string uuid;
        uint32_t revision = 0;
        uint32_t savedRevision = 0;
    };

    SceneInfoData DecodeSceneInfo(const Frame &frame)
    {
        EXPECT_EQ(frame.type, SceneInfoType) << frame.Text();
        SceneInfoData info;
        if (frame.type != SceneInfoType)
            return info;
        BufferReader r(frame.payload);
        info.path = r.ReadString();
        info.name = r.ReadString();
        info.uuid = r.ReadString();
        info.revision = r.ReadU32();
        info.savedRevision = r.ReadU32();
        return info;
    }

    /// Events of one kind the server pushed since the ring began
    std::vector<json> EventsOfKind(const EditorServer &server, const std::string &kind)
    {
        std::vector<json> found;
        for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
        {
            if (event.value("kind", "") == kind)
                found.push_back(event);
        }
        return found;
    }

    json LastEventOfKind(const EditorServer &server, const std::string &kind)
    {
        const std::vector<json> events = EventsOfKind(server, kind);
        return events.empty() ? json::object() : events.back();
    }

    json SceneJson()
    {
        return SceneManager::GetCurSceneRef().Serialize();
    }

    /// The commands every test below uses, on the server a derived fixture owns
    class HistoryTestBase : public ::testing::Test
    {
    protected:
        virtual EditorServer &Server() = 0;

        std::string Create(const std::string &name, const std::string &parentId = "", const std::string &preset = "")
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString(parentId);
            w.WriteI32(-1);
            w.WriteString(preset);
            const Frame created = Execute(Server(), CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        Frame Destroy(const std::string &id) { return Execute(Server(), CommandType::DestroyEntity, Strings({id})); }

        Frame SetLocal(const std::string &id, const std::array<float, 3> &position, const std::array<float, 3> &scale = {1.0f, 1.0f, 1.0f})
        {
            BufferWriter w;
            w.WriteString(id);
            for (const float v : position)
                w.WriteF32(v);
            for (const float v : {0.0f, 0.0f, 0.0f, 1.0f}) // the rotation x, y, z, w
                w.WriteF32(v);
            for (const float v : scale)
                w.WriteF32(v);
            return Execute(Server(), CommandType::SetLocalTransform, w.Release());
        }

        Frame SetProperties(const std::string &id, const json &properties)
        {
            BufferWriter w;
            w.WriteString(id);
            WriteJson(w, properties);
            return Execute(Server(), CommandType::SetEntityProperties, w.Release());
        }

        Frame Reparent(const std::string &id, const std::string &parentId, int32_t siblingIndex, bool keepWorld = true)
        {
            BufferWriter w;
            w.WriteString(id);
            w.WriteString(parentId);
            w.WriteI32(siblingIndex);
            w.WriteBool(keepWorld);
            return Execute(Server(), CommandType::SetEntityParent, w.Release());
        }

        std::string AddHolder(const std::string &entityId)
        {
            const Frame added = Execute(Server(), CommandType::AddComponent, Strings({entityId, HolderType}));
            EXPECT_EQ(added.type, ComponentAddedType) << added.Text();
            if (added.type != ComponentAddedType)
                return {};
            BufferReader r(added.payload);
            return r.ReadString();
        }

        Frame SetFields(const std::string &entityId, const std::string &componentId, const json &values)
        {
            BufferWriter w;
            w.WriteString(entityId);
            w.WriteString(componentId);
            WriteJson(w, values);
            return Execute(Server(), CommandType::SetComponentFields, w.Release());
        }

        Frame RemoveComponent(const std::string &entityId, const std::string &componentId)
        {
            return Execute(Server(), CommandType::RemoveComponent, Strings({entityId, componentId}));
        }

        json Get(const std::string &entityId, const std::string &componentId)
        {
            const Frame frame = Execute(Server(), CommandType::GetComponent, Strings({entityId, componentId}));
            EXPECT_EQ(frame.type, ComponentDataType) << frame.Text();
            if (frame.type != ComponentDataType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        /// GetEntity's entity (header, transform, components)
        json Entity(const std::string &id)
        {
            const Frame frame = Execute(Server(), CommandType::GetEntity, Strings({id}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        bool Exists(const std::string &id)
        {
            return Execute(Server(), CommandType::GetEntity, Strings({id})).type == EntityDataType;
        }

        EditResultData Undo() { return DecodeEditResult(Execute(Server(), CommandType::Undo)); }
        EditResultData Redo() { return DecodeEditResult(Execute(Server(), CommandType::Redo)); }

        Frame BeginGroup(const std::string &label) { return Execute(Server(), CommandType::BeginEditGroup, Strings({label})); }
        Frame EndGroup() { return Execute(Server(), CommandType::EndEditGroup); }

        struct HistoryData
        {
            uint32_t cursor = 0;
            json entries = json::array();
        };

        HistoryData History()
        {
            const Frame frame = Execute(Server(), CommandType::GetHistory);
            EXPECT_EQ(frame.type, HistoryType) << frame.Text();
            HistoryData data;
            if (frame.type != HistoryType)
                return data;
            BufferReader r(frame.payload);
            data.cursor = r.ReadU32();
            data.entries = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return data;
        }

        std::vector<std::string> Labels()
        {
            std::vector<std::string> labels;
            for (const json &entry : History().entries)
                labels.push_back(entry.at("label").get<std::string>());
            return labels;
        }

        SceneInfoData OpenSceneInfo() { return DecodeSceneInfo(Execute(Server(), CommandType::GetOpenScene)); }

        void ExpectError(const Frame &frame, const std::string &text)
        {
            EXPECT_EQ(frame.type, ErrorType) << frame.Text();
            EXPECT_NE(frame.Text().find(text), std::string::npos) << "expected '" << text << "' in: " << frame.Text();
        }

        /// Makes an edit, undoes it and redoes it (twice), checking the whole scene each time: the same as before
        /// after the undo, the same as after the edit after the redo, and the step's label
        template <typename Edit>
        void RoundTrip(const std::string &label, Edit edit)
        {
            const json before = SceneJson();
            edit();
            const json after = SceneJson();
            EXPECT_NE(before, after) << "the edit changed nothing: " << label;
            for (int round = 0; round < 2; ++round)
            {
                const EditResultData undone = Undo();
                EXPECT_EQ(undone.label, label);
                EXPECT_EQ(SceneJson(), before) << "after undoing " << label << " (round " << round << ")";
                EXPECT_TRUE(undone.canRedo);
                const EditResultData redone = Redo();
                EXPECT_EQ(redone.label, label);
                EXPECT_EQ(SceneJson(), after) << "after redoing " << label << " (round " << round << ")";
                EXPECT_FALSE(redone.canRedo);
                EXPECT_GT(redone.revision, undone.revision) << "the revision only grows";
            }
        }
    };

    /// A scene made by NewScene (no project needed), with coalescing off so that two edits in a row are two steps
    class EditorHistoryTest : public HistoryTestBase
    {
    protected:
        void SetUp() override
        {
            RegisterHolder();
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "History Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            server.GetHistory().SetCoalesceWindow(-1ms);
        }

        EditorServer &Server() override { return server; }

        EditorServer server;
    };
}

// ==================== Nothing to undo ====================

TEST_F(EditorHistoryTest, ANewSceneHasNothingToUndoOrRedo)
{
    ExpectError(Execute(server, CommandType::Undo), "Nothing to undo");
    ExpectError(Execute(server, CommandType::Redo), "Nothing to redo");
    const HistoryData history = History();
    EXPECT_EQ(history.cursor, 0u);
    EXPECT_TRUE(history.entries.empty());
}

TEST_F(EditorHistoryTest, ARefusedEditIsNotAStep)
{
    EXPECT_EQ(Destroy(Math::UUID::Random().ToString()).type, ErrorType);
    EXPECT_EQ(SetLocal(Math::UUID::Random().ToString(), {1.0f, 2.0f, 3.0f}).type, ErrorType);
    EXPECT_EQ(SetProperties(Math::UUID::Random().ToString(), json{{"name", "x"}}).type, ErrorType);
    const std::string entity = Create("A");
    EXPECT_EQ(SetProperties(entity, json{{"layer", 99}}).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::AddComponent, Strings({entity, "NoSuchComponent"})).type, ErrorType);
    EXPECT_EQ(SetFields(entity, Math::UUID::Random().ToString(), json{{"count", 1}}).type, ErrorType);
    EXPECT_EQ(History().entries.size(), 1u) << "only the Create";
}

TEST_F(EditorHistoryTest, AnEditThatChangesNothingIsNotAStep)
{
    const std::string entity = Create("A");
    ASSERT_EQ(SetLocal(entity, {1.0f, 2.0f, 3.0f}).type, OkType);
    const size_t steps = History().entries.size();
    EXPECT_EQ(SetLocal(entity, {1.0f, 2.0f, 3.0f}).type, OkType);
    EXPECT_EQ(SetProperties(entity, json{{"name", "A"}}).type, OkType);
    const std::string component = AddHolder(entity);
    const size_t withComponent = History().entries.size();
    EXPECT_EQ(SetFields(entity, component, json{{"count", 1}}).type, ComponentDataType);
    EXPECT_EQ(steps + 1, withComponent);
    EXPECT_EQ(History().entries.size(), withComponent);
}

// ==================== Every mutating command ====================

TEST_F(EditorHistoryTest, CreateEntityExIsUndoneAndRedone)
{
    std::string id;
    RoundTrip("Create Thing", [&] { id = Create("Thing"); });
    EXPECT_TRUE(Exists(id)) << "redone: the same UUID";

    // A preset's components come back with their UUIDs
    std::string cube;
    RoundTrip("Create Cube", [&] { cube = Create("", "", "Cube"); });
    ASSERT_TRUE(Exists(cube));
    EXPECT_EQ(Entity(cube).at("components").size(), 1u);
}

TEST_F(EditorHistoryTest, CreateEntityUndoDestroysTheObjectAndItsChildren)
{
    const std::string parent = Create("Parent");
    const std::string child = Create("Child", parent);
    EXPECT_EQ(Undo().label, "Create Child");
    EXPECT_FALSE(Exists(child));
    EXPECT_TRUE(Exists(parent));
    EXPECT_EQ(Undo().label, "Create Parent");
    EXPECT_FALSE(Exists(parent));
    EXPECT_EQ(Redo().label, "Create Parent");
    EXPECT_EQ(Redo().label, "Create Child");
    EXPECT_EQ(Entity(child).at("header").at("parentId"), parent);
}

TEST_F(EditorHistoryTest, TheLegacyCreateEntityIsUndoneAndRedone)
{
    std::string id;
    RoundTrip("Create Legacy", [&]
    {
        const Frame created = Execute(server, CommandType::CreateEntity, Strings({"Legacy"}));
        ASSERT_EQ(created.type, EntityCreatedType) << created.Text();
        BufferReader r(created.payload);
        id = r.ReadString();
    });
    EXPECT_TRUE(Exists(id));
}

TEST_F(EditorHistoryTest, DuplicateEntityIsUndoneAndRedoneWithItsReferencesToTheOutside)
{
    const std::string target = Create("Target");
    const std::string source = Create("Source");
    const std::string child = Create("SourceChild", source);
    const std::string holder = AddHolder(child);
    ASSERT_EQ(SetFields(child, holder, json{{"target", target}}).type, ComponentDataType);

    std::string copy;
    RoundTrip("Duplicate Source", [&]
    {
        const Frame duplicated = Execute(server, CommandType::DuplicateEntity, Strings({source}));
        ASSERT_EQ(duplicated.type, EntityCreatedType) << duplicated.Text();
        BufferReader r(duplicated.payload);
        copy = r.ReadString();
    });

    ASSERT_TRUE(Exists(copy));
    // The copy's own child points where the original did: outside the copy, so the reference is kept
    const json copied = Entity(copy);
    EXPECT_EQ(copied.at("header").at("name"), "Source (1)");
    const GameObject *copyObject = SceneManager::GetCurSceneRef().FindGameObjectByUUID(*Math::UUID::FromString(copy)).get();
    ASSERT_NE(copyObject, nullptr);
    ASSERT_EQ(copyObject->GetChildren().size(), 1u);
    const auto *copiedHolder = copyObject->GetChild(0)->GetComponent<HistoryHolder>();
    ASSERT_NE(copiedHolder, nullptr);
    ASSERT_NE(copiedHolder->target, nullptr);
    EXPECT_EQ(copiedHolder->target->GetUUID().ToString(), target);
}

TEST_F(EditorHistoryTest, DestroyEntityIsUndoneAndRedone)
{
    const std::string parent = Create("Parent");
    const std::string child = Create("Child", parent);
    Create("Sibling");
    RoundTrip("Delete Parent", [&] { EXPECT_EQ(Destroy(parent).type, OkType); });
    // The round trip ends redone: destroyed
    EXPECT_FALSE(Exists(parent));
    EXPECT_FALSE(Exists(child));
    EXPECT_EQ(Undo().label, "Delete Parent");
    EXPECT_TRUE(Exists(parent));
    EXPECT_TRUE(Exists(child));
    EXPECT_EQ(Entity(child).at("header").at("parentId"), parent);
}

TEST_F(EditorHistoryTest, UndoingADestroyPutsTheReferencesToTheObjectBack)
{
    const std::string holderEntity = Create("Holder");
    const std::string target = Create("Target");
    const std::string child = Create("TargetChild", target);
    const std::string holder = AddHolder(holderEntity);
    ASSERT_EQ(SetFields(holderEntity, holder, json{{"target", target}}).type, ComponentDataType);
    ASSERT_EQ(Get(holderEntity, holder).at("target"), target);
    const json before = SceneJson();

    ASSERT_EQ(Destroy(target).type, OkType);
    EXPECT_TRUE(Get(holderEntity, holder).at("target").is_null()) << "nothing keeps a pointer to the destroyed object";
    EXPECT_FALSE(Exists(child));
    const json after = SceneJson();

    EXPECT_EQ(Undo().label, "Delete Target");
    EXPECT_EQ(SceneJson(), before);
    EXPECT_EQ(Get(holderEntity, holder).at("target"), target) << "the reference is resolved again by UUID";
    EXPECT_TRUE(Exists(child));
    // Every object was rebuilt: a client is told to refetch everything
    EXPECT_TRUE(LastEventOfKind(server, "sceneChanged").value("full", false));

    EXPECT_EQ(Redo().label, "Delete Target");
    EXPECT_EQ(SceneJson(), after);
    EXPECT_TRUE(Get(holderEntity, holder).at("target").is_null());
    EXPECT_TRUE(LastEventOfKind(server, "sceneChanged").value("entityIds", json::array()).size() == 2u)
        << "redoing a destroy lists the objects it destroyed";
}

TEST_F(EditorHistoryTest, UndoingADestroyKeepsTheSceneNameUuidAndEditMode)
{
    const std::string entity = Create("Doomed");
    const Scene &scene = SceneManager::GetCurSceneRef();
    const std::string uuid = scene.GetUUID().ToString();
    const std::string name = scene.sceneName;
    ASSERT_EQ(Destroy(entity).type, OkType);
    Undo();

    const Scene &restored = SceneManager::GetCurSceneRef();
    EXPECT_EQ(restored.GetUUID().ToString(), uuid);
    EXPECT_EQ(restored.sceneName, name);
    EXPECT_TRUE(restored.IsEditMode());
    const SceneInfoData info = OpenSceneInfo();
    EXPECT_EQ(info.uuid, uuid);
    EXPECT_EQ(info.name, name);
}

TEST_F(EditorHistoryTest, SetLocalTransformIsUndoneAndRedone)
{
    const std::string entity = Create("Mover");
    RoundTrip("Transform Mover", [&] { EXPECT_EQ(SetLocal(entity, {1.0f, 2.0f, 3.0f}, {2.0f, 2.0f, 2.0f}).type, OkType); });
    EXPECT_EQ(Entity(entity).at("transform").at("position").at("y").get<float>(), 2.0f);
}

TEST_F(EditorHistoryTest, SettingATransformOnAnObjectWithoutOneIsUndoneToTheIdentity)
{
    const Frame created = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    ASSERT_EQ(created.type, EntityCreatedType);
    BufferReader r(created.payload);
    const std::string entity = r.ReadString();
    EXPECT_FALSE(Entity(entity).contains("transform"));

    ASSERT_EQ(SetLocal(entity, {5.0f, 6.0f, 7.0f}).type, OkType);
    EXPECT_EQ(Entity(entity).at("transform").at("position").at("x").get<float>(), 5.0f);
    EXPECT_EQ(Undo().label, "Transform Bare");
    // A transform can't be taken away: the object has the identity, where an object without one is
    const json transform = Entity(entity).at("transform");
    EXPECT_EQ(transform.at("position").at("x").get<float>(), 0.0f);
    EXPECT_EQ(transform.at("scale").at("x").get<float>(), 1.0f);
    EXPECT_EQ(Redo().label, "Transform Bare");
    EXPECT_EQ(Entity(entity).at("transform").at("position").at("x").get<float>(), 5.0f);
}

TEST_F(EditorHistoryTest, TheLegacySetEntityTransformIsUndoneAndRedone)
{
    const std::string entity = Create("Legacy Mover");
    RoundTrip("Transform Legacy Mover", [&]
    {
        BufferWriter w;
        w.WriteString(entity);
        for (const float v : {4.0f, 5.0f, 6.0f, 0.0f, 0.5f, 0.0f, 3.0f, 3.0f, 3.0f})
            w.WriteF32(v);
        EXPECT_EQ(Execute(server, CommandType::SetEntityTransform, w.Release()).type, OkType);
    });
}

TEST_F(EditorHistoryTest, SetEntityParentIsUndoneAndRedoneWithItsPlaceAndTransform)
{
    const std::string parent = Create("Parent");
    ASSERT_EQ(SetLocal(parent, {10.0f, 0.0f, 0.0f}).type, OkType);
    const std::string first = Create("First");
    const std::string second = Create("Second");
    const std::string third = Create("Third");
    ASSERT_EQ(SetLocal(second, {1.0f, 1.0f, 1.0f}).type, OkType);

    // Under the parent, first of its children, keeping the world position: the local one changes
    RoundTrip("Reparent Second", [&] { EXPECT_EQ(Reparent(second, parent, 0).type, OkType); });
    EXPECT_EQ(Entity(second).at("header").at("parentId"), parent);
    EXPECT_EQ(Entity(second).at("transform").at("position").at("x").get<float>(), -9.0f);

    // And back to the roots, between First and Third
    ASSERT_EQ(Undo().label, "Reparent Second");
    RoundTrip("Reparent Second", [&] { EXPECT_EQ(Reparent(second, "", 1).type, OkType); });
    (void)first;
    (void)third;
}

TEST_F(EditorHistoryTest, ReorderingWithinOneParentIsUndone)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    const std::string c = Create("C");
    RoundTrip("Reparent C", [&] { EXPECT_EQ(Reparent(c, "", 0).type, OkType); });
    EXPECT_EQ(Entity(c).at("header").at("index").get<int>(), 0);
    (void)a;
    (void)b;
}

TEST_F(EditorHistoryTest, SetEntityPropertiesIsUndoneAndRedone)
{
    const std::string entity = Create("Old");
    const std::string child = Create("Child", entity);

    RoundTrip("Rename Old", [&] { EXPECT_EQ(SetProperties(entity, json{{"name", "New"}}).type, OkType); });

    // The name is New now
    RoundTrip("Edit New", [&]
    {
        EXPECT_EQ(SetProperties(entity, json{{"tag", "Player"}, {"layer", 3}, {"active", false}}).type, OkType);
    });
    // Undone: active again, and so are its children in the hierarchy
    Undo();
    EXPECT_TRUE(Entity(child).at("header").at("activeInHierarchy").get<bool>());
    EXPECT_EQ(Entity(entity).at("header").at("tag"), "Untagged");
    Redo();
    EXPECT_FALSE(Entity(child).at("header").at("activeInHierarchy").get<bool>());
    EXPECT_EQ(Entity(entity).at("header").at("layer").get<int>(), 3);
}

TEST_F(EditorHistoryTest, AddComponentIsUndoneAndRedoneWithTheSameUuid)
{
    const std::string entity = Create("Host");
    std::string component;
    RoundTrip(std::string("Add ") + HolderType, [&] { component = AddHolder(entity); });
    EXPECT_EQ(Get(entity, component).at("count"), 1) << "redone: the component the first add made";
}

TEST_F(EditorHistoryTest, AddComponentRedoKeepsTheValuesItHadWhenAdded)
{
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    ASSERT_EQ(SetFields(entity, component, json{{"count", 7}}).type, ComponentDataType);
    Undo(); // the field edit
    Undo(); // the add
    EXPECT_EQ(Execute(server, CommandType::GetComponent, Strings({entity, component})).type, ErrorType);
    Redo();
    EXPECT_EQ(Get(entity, component).at("count"), 1);
    Redo();
    EXPECT_EQ(Get(entity, component).at("count"), 7);
}

TEST_F(EditorHistoryTest, RemoveComponentIsUndoneAndRedoneWithTheReferencesToIt)
{
    const std::string firstEntity = Create("First");
    const std::string secondEntity = Create("Second");
    const std::string first = AddHolder(firstEntity);
    const std::string second = AddHolder(secondEntity);
    ASSERT_EQ(SetFields(secondEntity, second, json{{"peer", first}}).type, ComponentDataType);
    ASSERT_EQ(Get(secondEntity, second).at("peer"), first);
    const json before = SceneJson();

    ASSERT_EQ(RemoveComponent(firstEntity, first).type, OkType);
    EXPECT_TRUE(Get(secondEntity, second).at("peer").is_null()) << "the removal cleared the reference";
    const json after = SceneJson();

    EXPECT_EQ(Undo().label, std::string("Remove ") + HolderType);
    EXPECT_EQ(SceneJson(), before);
    EXPECT_EQ(Get(secondEntity, second).at("peer"), first) << "put back, by UUID";
    EXPECT_TRUE(LastEventOfKind(server, "sceneChanged").value("full", false));

    EXPECT_EQ(Redo().label, std::string("Remove ") + HolderType);
    EXPECT_EQ(SceneJson(), after);
    EXPECT_TRUE(Get(secondEntity, second).at("peer").is_null());
}

TEST_F(EditorHistoryTest, RemoveComponentPushesSceneChangedWithTheObjectsWhoseReferencesItCleared)
{
    const std::string firstEntity = Create("First");
    const std::string secondEntity = Create("Second");
    const std::string thirdEntity = Create("Third");
    const std::string first = AddHolder(firstEntity);
    const std::string second = AddHolder(secondEntity);
    AddHolder(thirdEntity);
    ASSERT_EQ(SetFields(secondEntity, second, json{{"peer", first}}).type, ComponentDataType);

    ASSERT_EQ(RemoveComponent(firstEntity, first).type, OkType);
    const json removed = LastEventOfKind(server, "sceneChanged");
    ASSERT_TRUE(removed.contains("entityIds"));
    EXPECT_EQ(removed.at("entityIds"), (json::array({firstEntity, secondEntity}))) << "the object it was on, and the holder of the reference";
    EXPECT_FALSE(removed.contains("full"));

    // Redo clears the reference again, and says so the same way
    Undo();
    Redo();
    const json redone = LastEventOfKind(server, "sceneChanged");
    ASSERT_TRUE(redone.contains("entityIds"));
    EXPECT_EQ(redone.at("entityIds"), (json::array({firstEntity, secondEntity})));
}

TEST_F(EditorHistoryTest, RemovingAComponentNothingReferencesListsOnlyItsObject)
{
    const std::string firstEntity = Create("First");
    const std::string secondEntity = Create("Second");
    const std::string first = AddHolder(firstEntity);
    AddHolder(secondEntity);

    ASSERT_EQ(RemoveComponent(firstEntity, first).type, OkType);
    EXPECT_EQ(LastEventOfKind(server, "sceneChanged").at("entityIds"), (json::array({firstEntity})));
}

TEST_F(EditorHistoryTest, DestroyEntityPushesSceneChangedWithTheObjectsWhoseReferencesItCleared)
{
    const std::string firstEntity = Create("First");
    const std::string secondEntity = Create("Second");
    const std::string thirdEntity = Create("Third");
    const std::string first = AddHolder(firstEntity);
    const std::string second = AddHolder(secondEntity);
    AddHolder(thirdEntity);
    ASSERT_EQ(SetFields(secondEntity, second, json{{"peer", first}}).type, ComponentDataType);

    ASSERT_EQ(Destroy(firstEntity).type, OkType);
    const json destroyed = LastEventOfKind(server, "sceneChanged");
    ASSERT_TRUE(destroyed.contains("entityIds"));
    EXPECT_EQ(destroyed.at("entityIds"), (json::array({firstEntity, secondEntity})));

    Undo();
    Redo();
    const json redone = LastEventOfKind(server, "sceneChanged");
    ASSERT_TRUE(redone.contains("entityIds"));
    EXPECT_EQ(redone.at("entityIds"), (json::array({firstEntity, secondEntity})));
}

TEST_F(EditorHistoryTest, SetComponentFieldsIsUndoneAndRedone)
{
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    RoundTrip("Set count", [&] { EXPECT_EQ(SetFields(entity, component, json{{"count", 5}}).type, ComponentDataType); });
    EXPECT_EQ(Get(entity, component).at("count"), 5);

    RoundTrip("Edit " + std::string(HolderType), [&]
    {
        EXPECT_EQ(SetFields(entity, component, json{{"count", 9}, {"label", "changed"}}).type, ComponentDataType);
    });
}

TEST_F(EditorHistoryTest, AFieldClampedOnSetIsUndoneToWhatItWasAndRedoneToWhatWasStored)
{
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    // The range is 0 to 10: 99 is stored as 10
    ASSERT_EQ(SetFields(entity, component, json{{"count", 99}}).type, ComponentDataType);
    ASSERT_EQ(Get(entity, component).at("count"), 10);
    Undo();
    EXPECT_EQ(Get(entity, component).at("count"), 1);
    Redo();
    EXPECT_EQ(Get(entity, component).at("count"), 10);
}

TEST_F(EditorHistoryTest, ReferenceFieldsAreUndoneAndRedone)
{
    const std::string entity = Create("Host");
    const std::string other = Create("Other");
    const std::string component = AddHolder(entity);
    const std::string peerComponent = AddHolder(other);
    RoundTrip("Set target", [&] { EXPECT_EQ(SetFields(entity, component, json{{"target", other}}).type, ComponentDataType); });
    RoundTrip("Set peer", [&] { EXPECT_EQ(SetFields(entity, component, json{{"peer", peerComponent}}).type, ComponentDataType); });
}

// ==================== Order, labels, events ====================

TEST_F(EditorHistoryTest, StepsUndoLatestFirstAndTheHistoryListsThem)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    SetProperties(b, json{{"name", "Bee"}});

    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create A", "Create B", "Transform A", "Rename B"}));
    EXPECT_EQ(History().cursor, 4u);
    EXPECT_GT(History().entries.at(0).at("bytes").get<int>(), 0);

    EXPECT_EQ(Undo().label, "Rename B");
    EXPECT_EQ(Undo().label, "Transform A");
    EXPECT_EQ(History().cursor, 2u);
    EXPECT_EQ(History().entries.size(), 4u) << "the undone steps stay listed, for a redo";

    // A new edit discards what could have been redone
    SetProperties(a, json{{"tag", "Tagged"}});
    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create A", "Create B", "Edit A"}));
    ExpectError(Execute(server, CommandType::Redo), "Nothing to redo");
}

TEST_F(EditorHistoryTest, EditResultSaysWhatCanBeUndoneAndRedone)
{
    Create("A");
    Create("B");
    const EditResultData first = Undo();
    EXPECT_EQ(first.label, "Create B");
    EXPECT_TRUE(first.canUndo);
    EXPECT_TRUE(first.canRedo);
    const EditResultData second = Undo();
    EXPECT_FALSE(second.canUndo);
    EXPECT_TRUE(second.canRedo);
    EXPECT_EQ(second.revision, OpenSceneInfo().revision);
    const EditResultData redone = Redo();
    EXPECT_TRUE(redone.canUndo);
    EXPECT_TRUE(redone.canRedo);
}

TEST_F(EditorHistoryTest, HistoryChangedIsPushedForEveryStepUndoAndRedo)
{
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), 1u) << "the new scene's empty history";
    Create("A");
    json event = LastEventOfKind(server, "historyChanged");
    EXPECT_TRUE(event.at("canUndo").get<bool>());
    EXPECT_FALSE(event.at("canRedo").get<bool>());
    EXPECT_EQ(event.at("label"), "Create A");
    EXPECT_EQ(event.at("redoLabel"), "");
    EXPECT_EQ(event.at("undoCount").get<int>(), 1);
    EXPECT_EQ(event.at("redoCount").get<int>(), 0);

    Undo();
    event = LastEventOfKind(server, "historyChanged");
    EXPECT_FALSE(event.at("canUndo").get<bool>());
    EXPECT_TRUE(event.at("canRedo").get<bool>());
    EXPECT_EQ(event.at("label"), "");
    EXPECT_EQ(event.at("redoLabel"), "Create A");
    EXPECT_EQ(event.at("redoCount").get<int>(), 1);

    Redo();
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), 4u);
}

TEST_F(EditorHistoryTest, UndoAndRedoPushSceneChangedWithTheObjectsTheyTouched)
{
    const std::string a = Create("A");
    const std::string b = Create("B", a);
    SetLocal(a, {1.0f, 0.0f, 0.0f});

    const EditResultData undone = Undo();
    const json event = LastEventOfKind(server, "sceneChanged");
    EXPECT_EQ(event.at("revision").get<uint32_t>(), undone.revision);
    EXPECT_FALSE(event.contains("full"));
    // The transform moved A and, with it, B
    ASSERT_TRUE(event.contains("entityIds"));
    EXPECT_EQ(event.at("entityIds").size(), 2u);
    EXPECT_EQ(event.at("entityIds").at(0), a);
    EXPECT_EQ(event.at("entityIds").at(1), b);
}

TEST_F(EditorHistoryTest, TheRevisionOnlyGrowsThroughUndoAndRedo)
{
    Create("A");
    const uint32_t created = OpenSceneInfo().revision;
    const uint32_t undone = Undo().revision;
    EXPECT_GT(undone, created);
    const uint32_t redone = Redo().revision;
    EXPECT_GT(redone, undone);
    EXPECT_EQ(OpenSceneInfo().revision, redone);
}

// ==================== The saved state ====================

TEST_F(EditorHistoryTest, UndoingBackToTheStateTheSceneWasOpenedAtIsNoUnsavedChange)
{
    EXPECT_EQ(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);
    Create("A");
    Create("B");
    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);

    const EditResultData one = Undo();
    EXPECT_NE(one.revision, one.savedRevision);
    const EditResultData none = Undo();
    EXPECT_EQ(none.revision, none.savedRevision) << "back at the new scene";
    EXPECT_EQ(OpenSceneInfo().savedRevision, OpenSceneInfo().revision);
    EXPECT_EQ(LastEventOfKind(server, "sceneChanged").at("savedRevision"), LastEventOfKind(server, "sceneChanged").at("revision"));

    const EditResultData again = Redo();
    EXPECT_NE(again.revision, again.savedRevision);
}

// ==================== Coalescing ====================

TEST_F(EditorHistoryTest, TypingInOneFieldIsOneStep)
{
    server.GetHistory().SetCoalesceWindow(1h);
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    const size_t steps = History().entries.size();

    for (const int value : {2, 3, 4, 5})
        ASSERT_EQ(SetFields(entity, component, json{{"count", value}}).type, ComponentDataType);
    EXPECT_EQ(History().entries.size(), steps + 1);
    EXPECT_EQ(Get(entity, component).at("count"), 5);

    EXPECT_EQ(Undo().label, "Set count");
    EXPECT_EQ(Get(entity, component).at("count"), 1) << "back to before the first keystroke";
    Redo();
    EXPECT_EQ(Get(entity, component).at("count"), 5);

    // Another field is another step
    ASSERT_EQ(SetFields(entity, component, json{{"label", "x"}}).type, ComponentDataType);
    EXPECT_EQ(History().entries.size(), steps + 2);
}

TEST_F(EditorHistoryTest, DraggingOneObjectWithoutAGroupIsOneStepToo)
{
    server.GetHistory().SetCoalesceWindow(1h);
    const std::string a = Create("A");
    const std::string b = Create("B");
    const size_t steps = History().entries.size();
    for (const float x : {1.0f, 2.0f, 3.0f})
        ASSERT_EQ(SetLocal(a, {x, 0.0f, 0.0f}).type, OkType);
    EXPECT_EQ(History().entries.size(), steps + 1);
    ASSERT_EQ(SetLocal(b, {1.0f, 0.0f, 0.0f}).type, OkType);
    EXPECT_EQ(History().entries.size(), steps + 2) << "another object";

    Undo();
    Undo();
    EXPECT_EQ(Entity(a).at("transform").at("position").at("x").get<float>(), 0.0f);
}

TEST_F(EditorHistoryTest, TypingANameIsOneStep)
{
    server.GetHistory().SetCoalesceWindow(1h);
    const std::string entity = Create("Old");
    const size_t steps = History().entries.size();
    for (const std::string name : {"N", "Ne", "New"})
        ASSERT_EQ(SetProperties(entity, json{{"name", name}}).type, OkType);
    EXPECT_EQ(History().entries.size(), steps + 1);
    EXPECT_EQ(Undo().label, "Rename Old");
    EXPECT_EQ(Entity(entity).at("header").at("name"), "Old");
}

TEST_F(EditorHistoryTest, EditsFarApartInTimeAreNotCoalesced)
{
    server.GetHistory().SetCoalesceWindow(-1ms);
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    const size_t steps = History().entries.size();
    ASSERT_EQ(SetFields(entity, component, json{{"count", 2}}).type, ComponentDataType);
    ASSERT_EQ(SetFields(entity, component, json{{"count", 3}}).type, ComponentDataType);
    EXPECT_EQ(History().entries.size(), steps + 2);
}

TEST_F(EditorHistoryTest, ACoalescedEditStillPushesSceneChangedButNoNewHistoryEvent)
{
    server.GetHistory().SetCoalesceWindow(1h);
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    ASSERT_EQ(SetFields(entity, component, json{{"count", 2}}).type, ComponentDataType);
    const size_t historyEvents = EventsOfKind(server, "historyChanged").size();
    const size_t sceneEvents = EventsOfKind(server, "sceneChanged").size();
    ASSERT_EQ(SetFields(entity, component, json{{"count", 3}}).type, ComponentDataType);
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), historyEvents);
    EXPECT_EQ(EventsOfKind(server, "sceneChanged").size(), sceneEvents + 1);
}

// ==================== Groups ====================

TEST_F(EditorHistoryTest, AGroupIsOneStepForEveryEditInIt)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    const json before = SceneJson();
    const size_t eventsBefore = EventsOfKind(server, "historyChanged").size();

    ASSERT_EQ(BeginGroup("Move both").type, OkType);
    // CanUndo and CanRedo are false while a group is open: a menu is told
    const size_t historyEvents = EventsOfKind(server, "historyChanged").size();
    EXPECT_EQ(historyEvents, eventsBefore + 1);
    EXPECT_FALSE(LastEventOfKind(server, "historyChanged").at("canUndo").get<bool>());
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(b, {2.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(a, {3.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetProperties(a, json{{"tag", "Moved"}}).type, OkType);
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), historyEvents) << "nothing is a step until the group ends";
    EXPECT_EQ(History().entries.size(), 2u);
    ASSERT_EQ(EndGroup().type, OkType);
    const json after = SceneJson();

    EXPECT_EQ(History().entries.size(), 3u);
    EXPECT_EQ(Labels().back(), "Move both");
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), historyEvents + 1);
    EXPECT_NE(before, after);

    EXPECT_EQ(Undo().label, "Move both");
    EXPECT_EQ(SceneJson(), before);
    EXPECT_EQ(Redo().label, "Move both");
    EXPECT_EQ(SceneJson(), after);
    EXPECT_EQ(Entity(a).at("transform").at("position").at("x").get<float>(), 3.0f);
}

TEST_F(EditorHistoryTest, AGroupWithADestroyAndACreateUndoesInReverse)
{
    const std::string a = Create("A");
    const json before = SceneJson();
    BeginGroup("Replace");
    ASSERT_EQ(Destroy(a).type, OkType);
    const std::string replacement = Create("A2");
    EndGroup();
    const json after = SceneJson();

    EXPECT_EQ(Undo().label, "Replace");
    EXPECT_EQ(SceneJson(), before);
    EXPECT_TRUE(Exists(a));
    EXPECT_FALSE(Exists(replacement));
    EXPECT_EQ(Redo().label, "Replace");
    EXPECT_EQ(SceneJson(), after);
    EXPECT_FALSE(Exists(a));
    EXPECT_TRUE(Exists(replacement));
}

TEST_F(EditorHistoryTest, GroupsNestAndTheOuterOneNamesTheStep)
{
    const std::string a = Create("A");
    BeginGroup("Outer");
    BeginGroup("Inner");
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    EndGroup();
    SetProperties(a, json{{"tag", "T"}});
    EXPECT_EQ(History().entries.size(), 1u) << "the inner group's end doesn't end the step";
    EndGroup();
    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create A", "Outer"}));
    Undo();
    EXPECT_EQ(Entity(a).at("header").at("tag"), "Untagged");
}

TEST_F(EditorHistoryTest, AnEmptyGroupMakesNoStep)
{
    const size_t historyEvents = EventsOfKind(server, "historyChanged").size();
    BeginGroup("Nothing");
    EXPECT_EQ(EndGroup().type, OkType);
    EXPECT_TRUE(History().entries.empty());
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), historyEvents + 2) << "opened and closed";
}

TEST_F(EditorHistoryTest, AGroupWhoseWritesCancelOutMakesNoStepAndKeepsTheRedoSteps)
{
    const std::string a = Create("A");
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    Create("B");
    Undo();
    ASSERT_EQ(History().entries.size(), 3u);
    const json before = SceneJson();
    const size_t historyEvents = EventsOfKind(server, "historyChanged").size();

    // A gizmo drag, then Esc writes the first value back
    ASSERT_EQ(BeginGroup("Move").type, OkType);
    ASSERT_EQ(SetLocal(a, {5.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(a, {9.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetProperties(a, json{{"tag", "T"}}).type, OkType);
    ASSERT_EQ(SetProperties(a, json{{"tag", "Untagged"}}).type, OkType);
    ASSERT_EQ(EndGroup().type, OkType);

    EXPECT_EQ(SceneJson(), before);
    EXPECT_EQ(History().entries.size(), 3u) << "no step for it";
    EXPECT_EQ(History().cursor, 2u);
    EXPECT_TRUE(LastEventOfKind(server, "historyChanged").at("canRedo").get<bool>()) << "the redo step is still there";
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), historyEvents + 2) << "opened and closed";
    EXPECT_EQ(Redo().label, "Create B");
}

TEST_F(EditorHistoryTest, AGroupKeepsTheOpsThatChangedSomethingAndDropsTheOnesThatDidNot)
{
    const std::string a = Create("A");
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    const json before = SceneJson();

    ASSERT_EQ(BeginGroup("Edit").type, OkType);
    ASSERT_EQ(SetLocal(a, {5.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetProperties(a, json{{"tag", "Moved"}}).type, OkType);
    ASSERT_EQ(EndGroup().type, OkType);

    EXPECT_EQ(History().entries.size(), 3u);
    EXPECT_EQ(Undo().label, "Edit");
    EXPECT_EQ(SceneJson(), before);
}

TEST_F(EditorHistoryTest, EndingAGroupThatIsntOpenIsAnError)
{
    ExpectError(EndGroup(), "No edit group is open");
    BeginGroup("x");
    EXPECT_EQ(EndGroup().type, OkType);
    ExpectError(EndGroup(), "No edit group is open");
}

TEST_F(EditorHistoryTest, UndoAndRedoAreRefusedWhileAGroupIsOpen)
{
    Create("A");
    ASSERT_EQ(Undo().label, "Create A");
    BeginGroup("Drag");
    ExpectError(Execute(server, CommandType::Undo), "group is open");
    ExpectError(Execute(server, CommandType::Redo), "group is open");
    EndGroup();
    EXPECT_EQ(Redo().label, "Create A") << "refused undo and redo changed nothing";
}

TEST_F(EditorHistoryTest, GroupsNestOnlySixteenDeep)
{
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(BeginGroup("g").type, OkType) << i;
    ExpectError(BeginGroup("too deep"), "nested");
}

TEST_F(EditorHistoryTest, AGroupLabelIsMadeSafeAndShort)
{
    const std::string a = Create("A");
    BeginGroup(std::string("line one\nline two ") + std::string(300, 'x'));
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    EndGroup();
    const std::string label = Labels().back();
    EXPECT_EQ(label.find('\n'), std::string::npos);
    EXPECT_LE(label.size(), 103u);

    BeginGroup("");
    SetLocal(a, {2.0f, 0.0f, 0.0f});
    EndGroup();
    EXPECT_EQ(Labels().back(), "Edit");
}

TEST_F(EditorHistoryTest, ANewHelloEndsAGroupTheLastSessionLeftOpen)
{
    const std::string a = Create("A");
    BeginGroup("Abandoned");
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    EXPECT_EQ(History().entries.size(), 1u);

    const Frame hello = Execute(server, CommandType::Hello, Strings({"test", std::string(ProtocolVersion), ""}));
    ASSERT_EQ(hello.type, ServerInfoType) << hello.Text();
    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create A", "Abandoned"}));
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(Undo().label, "Abandoned");
}

// ==================== Clearing ====================

TEST_F(EditorHistoryTest, OpeningANewSceneClearsTheHistory)
{
    Create("A");
    Create("B");
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Another"})).type, SceneInfoType);
    EXPECT_TRUE(History().entries.empty());
    ExpectError(Execute(server, CommandType::Undo), "Nothing to undo");
    const json event = LastEventOfKind(server, "historyChanged");
    EXPECT_FALSE(event.at("canUndo").get<bool>());
    EXPECT_FALSE(event.at("canRedo").get<bool>());
}

TEST_F(EditorHistoryTest, LoadSceneClearsTheHistoryAndTheSceneIsUnsaved)
{
    Create("A");
    const Frame loaded = Execute(server, CommandType::LoadScene, Strings({R"({"name":"Loaded","rootGameObjects":[]})"}));
    ASSERT_EQ(loaded.type, OkType) << loaded.Text();
    EXPECT_TRUE(History().entries.empty());
    ExpectError(Execute(server, CommandType::Undo), "Nothing to undo");
    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);

    // And edits to it undo, but never back to a saved state
    Create("B");
    const EditResultData undone = Undo();
    EXPECT_NE(undone.revision, undone.savedRevision);
}

// ==================== Limits ====================

TEST_F(EditorHistoryTest, TheServerKeepsAtMostTheHistoryLimit)
{
    server.GetHistory().SetLimits(3, 1024 * 1024);
    std::vector<std::string> ids;
    for (int i = 1; i <= 5; ++i)
        ids.push_back(Create("Object " + std::to_string(i)));
    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create Object 3", "Create Object 4", "Create Object 5"}));

    for (int i = 0; i < 3; ++i)
        Undo();
    ExpectError(Execute(server, CommandType::Undo), "Nothing to undo");
    EXPECT_TRUE(Exists(ids[0]));
    EXPECT_TRUE(Exists(ids[1]));
    EXPECT_FALSE(Exists(ids[2]));
}

TEST_F(EditorHistoryTest, SnapshotStepsCountTheirBytesAgainstTheLimit)
{
    for (int i = 0; i < 30; ++i)
        Create("Object " + std::to_string(i));
    const std::string victim = Create("Victim");
    const size_t before = server.GetHistory().TotalBytes();
    ASSERT_EQ(Destroy(victim).type, OkType);
    // The destroy holds a snapshot of the whole scene
    EXPECT_GT(server.GetHistory().TotalBytes() - before, 3000u);
    EXPECT_GT(History().entries.back().at("bytes").get<size_t>(), 3000u);

    // A small cap drops the older steps, never the newest
    server.GetHistory().SetLimits(200, 4000);
    EXPECT_EQ(History().entries.size(), 1u);
    EXPECT_EQ(Labels().back(), "Delete Victim");
    EXPECT_EQ(Undo().label, "Delete Victim");
    EXPECT_TRUE(Exists(victim));
}

// ==================== A step that can't be undone ====================

TEST_F(EditorHistoryTest, AStepThatCantBeUndoneClearsTheHistoryAndSaysSo)
{
    const std::string a = Create("A");
    Create("B");
    // The scene changed behind the history's back: A is gone, so undoing its creation can't work
    Scene &scene = SceneManager::GetCurSceneRef();
    const auto object = scene.FindGameObject("A");
    ASSERT_NE(object, nullptr);
    ASSERT_TRUE(scene.DestroyGameObject(object));
    scene.ProcessDestroyed();
    ASSERT_EQ(Undo().label, "Create B");

    const Frame failed = Execute(server, CommandType::Undo);
    ExpectError(failed, "Couldn't undo 'Create A'");
    EXPECT_NE(failed.Text().find("cleared"), std::string::npos);
    EXPECT_TRUE(History().entries.empty());
    EXPECT_TRUE(LastEventOfKind(server, "sceneChanged").value("full", false)) << "the client refetches everything";
    EXPECT_FALSE(LastEventOfKind(server, "historyChanged").at("canUndo").get<bool>());
    (void)a;
}

// ==================== Across snapshot restores ====================

TEST_F(EditorHistoryTest, StepsBeforeAndAfterASnapshotRestoreStillUndoAndRedo)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    const std::string c = Create("C");
    const std::string holder = AddHolder(a);
    std::vector<json> states{SceneJson()};

    SetLocal(a, {1.0f, 2.0f, 3.0f});
    states.push_back(SceneJson());
    ASSERT_EQ(Destroy(b).type, OkType);
    states.push_back(SceneJson());
    SetProperties(c, json{{"name", "Renamed"}});
    states.push_back(SceneJson());
    ASSERT_EQ(RemoveComponent(a, holder).type, OkType);
    states.push_back(SceneJson());
    SetLocal(a, {9.0f, 9.0f, 9.0f});
    states.push_back(SceneJson());

    for (size_t i = states.size() - 1; i > 0; --i)
    {
        Undo();
        EXPECT_EQ(SceneJson(), states[i - 1]) << "after undoing step " << i;
    }
    for (size_t i = 1; i < states.size(); ++i)
    {
        Redo();
        EXPECT_EQ(SceneJson(), states[i]) << "after redoing step " << i;
    }
}

// ==================== Edit mode ====================

namespace
{
    /// Counts the lifecycle callbacks it gets
    class HistoryProbe final : public Component
    {
    public:
        explicit HistoryProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "EditorHistoryTest_Probe"; }

        void OnEnable() override { ++calls; }
        void OnDisable() override { ++calls; }
        void OnDestroy() override { ++calls; }

        static inline int calls = 0;
    };
}

TEST_F(EditorHistoryTest, UndoAndRedoRunNoLifecycleCallbacksInAnEditModeScene)
{
    ComponentRegistry::Instance().Register(
        "EditorHistoryTest_Probe",
        [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<HistoryProbe>(gameObject); });
    const std::string entity = Create("Probed");
    HistoryProbe::calls = 0;

    const std::string probe = [&]
    {
        const Frame added = Execute(server, CommandType::AddComponent, Strings({entity, "EditorHistoryTest_Probe"}));
        BufferReader r(added.payload);
        return r.ReadString();
    }();
    ASSERT_EQ(RemoveComponent(entity, probe).type, OkType);
    Undo();
    Redo();
    Undo();
    Undo();
    Redo();
    EXPECT_EQ(HistoryProbe::calls, 0) << "nothing in an edit-mode scene was ever enabled";
}

// ==================== With a project ====================

namespace
{
    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    void WriteFile(const fs::path &path, const std::string &text)
    {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    /// A new project (IO::CreateProject) in a fresh temp folder, opened as RunHost opens one
    class EditorHistoryProjectTest : public HistoryTestBase
    {
    protected:
        void SetUp() override
        {
            RegisterHolder();
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-history-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            // Written after every step, so a test sees it at once
            server.SetAutosaveInterval(0ms);
            server.GetHistory().SetCoalesceWindow(-1ms);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        EditorServer &Server() override { return server; }

        SceneInfoData Open(const std::string &path)
        {
            return DecodeSceneInfo(Execute(server, CommandType::OpenScene, Strings({path})));
        }

        SceneInfoData Save(const std::string &path = "")
        {
            return DecodeSceneInfo(Execute(server, CommandType::SaveSceneToFile, Strings({path})));
        }

        [[nodiscard]] fs::path SceneFile() const { return _root / "assets" / "scenes" / "Main.scene"; }
        [[nodiscard]] fs::path AutosaveFile() const { return _root / ".n2" / "autosave" / "scenes" / "Main.scene"; }

        json AutosaveInfo()
        {
            const Frame frame = Execute(server, CommandType::GetAutosave);
            EXPECT_EQ(frame.type, AutosaveType) << frame.Text();
            if (frame.type != AutosaveType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        /// The names of the objects at the top of an autosave (or any scene file)
        static std::vector<std::string> RootNames(const std::string &text)
        {
            std::vector<std::string> names;
            const json scene = json::parse(text, nullptr, false);
            if (scene.is_discarded() || !scene.contains("rootGameObjects"))
                return names;
            for (const json &root : scene.at("rootGameObjects"))
                names.push_back(root.at("name").get<std::string>());
            return names;
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

TEST_F(EditorHistoryProjectTest, SavingKeepsTheHistoryAndUndoingBackToTheSavedStateIsClean)
{
    Open("res://scenes/Main.scene");
    Create("A");
    const SceneInfoData saved = Save();
    EXPECT_EQ(saved.revision, saved.savedRevision);
    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create A"})) << "kept across the save";

    Create("B");
    SetLocal(Create("C"), {1.0f, 0.0f, 0.0f});
    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);

    // Back to what was saved: no unsaved changes
    Undo();
    Undo();
    EditResultData atSaved = Undo();
    EXPECT_EQ(atSaved.label, "Create B");
    EXPECT_EQ(atSaved.revision, atSaved.savedRevision);
    EXPECT_EQ(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);

    // Before it (the step made before the save, undone after it): unsaved
    const EditResultData before = Undo();
    EXPECT_EQ(before.label, "Create A");
    EXPECT_NE(before.revision, before.savedRevision);
    const EditResultData forward = Redo();
    EXPECT_EQ(forward.revision, forward.savedRevision) << "and forward again";
    EXPECT_TRUE(forward.canRedo);
}

TEST_F(EditorHistoryProjectTest, AGroupWhoseWritesCancelOutLeavesTheSceneAsSavedAsItWas)
{
    Open("res://scenes/Main.scene");
    const std::string a = Create("A");
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    Save();
    ASSERT_EQ(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);

    BeginGroup("Move");
    ASSERT_EQ(SetLocal(a, {5.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision) << "the writes moved the revision";
    ASSERT_EQ(EndGroup().type, OkType);

    EXPECT_EQ(OpenSceneInfo().revision, OpenSceneInfo().savedRevision) << "the scene is the saved one again";
    const json event = LastEventOfKind(server, "sceneChanged");
    EXPECT_EQ(event.at("savedRevision"), event.at("revision"));
    EXPECT_EQ(History().entries.size(), 2u);
}

TEST_F(EditorHistoryProjectTest, AGroupWhoseWritesCancelOutDoesNotSaveAnUnsavedScene)
{
    Open("res://scenes/Main.scene");
    const std::string a = Create("A");
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);

    BeginGroup("Move");
    ASSERT_EQ(SetLocal(a, {5.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(SetLocal(a, {1.0f, 0.0f, 0.0f}).type, OkType);
    ASSERT_EQ(EndGroup().type, OkType);

    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision) << "the edits before the group are unsaved";
}

TEST_F(EditorHistoryProjectTest, ANewEditAfterUndoingPastTheSavedStateLosesIt)
{
    Open("res://scenes/Main.scene");
    Create("A");
    Create("B");
    Save();
    Undo(); // B: unsaved now
    Create("C"); // discards the redo of B, the saved state
    const EditResultData undone = Undo();
    EXPECT_EQ(undone.label, "Create C");
    EXPECT_NE(undone.revision, undone.savedRevision) << "no state of this branch is the one on disk";
    const EditResultData more = Undo();
    EXPECT_NE(more.revision, more.savedRevision);
}

TEST_F(EditorHistoryProjectTest, OpeningASceneClearsTheHistoryAndMarksTheOpenedStateSaved)
{
    Open("res://scenes/Main.scene");
    Create("A");
    Save();
    ASSERT_FALSE(Labels().empty());
    Open("res://scenes/Main.scene");
    EXPECT_TRUE(Labels().empty());
    EXPECT_EQ(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);
    Create("B");
    const EditResultData undone = Undo();
    EXPECT_EQ(undone.revision, undone.savedRevision);
}

// ==================== Autosave ====================

TEST_F(EditorHistoryProjectTest, TheAutosaveIsWrittenAtTheEndOfAStepAndNeverOverTheScenesFile)
{
    Open("res://scenes/Main.scene");
    const std::string fileBefore = ReadFile(SceneFile());
    EXPECT_FALSE(fs::exists(AutosaveFile()));
    EXPECT_FALSE(AutosaveInfo().at("exists").get<bool>());

    Create("Autosaved");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile())) << "under .n2/autosave/, at the scene's path under assets/";
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"Autosaved"}));
    EXPECT_EQ(ReadFile(SceneFile()), fileBefore) << "the scene's own file is untouched";

    const json info = AutosaveInfo();
    EXPECT_TRUE(info.at("exists").get<bool>());
    EXPECT_EQ(fs::path(info.at("path").get<std::string>()), AutosaveFile());
    EXPECT_EQ(info.at("size").get<uint64_t>(), fs::file_size(AutosaveFile()));
    EXPECT_GT(info.at("modified").get<int64_t>(), 0);

    // It follows the scene, and doesn't change what is unsaved
    Create("Second");
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"Autosaved", "Second"}));
    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);
    EXPECT_EQ(ReadFile(SceneFile()), fileBefore);
}

TEST_F(EditorHistoryProjectTest, TheAutosaveIsWrittenWhenAGroupEndsNotBefore)
{
    Open("res://scenes/Main.scene");
    const std::string a = Create("A");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    fs::remove(AutosaveFile());

    BeginGroup("Drag");
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    SetLocal(a, {2.0f, 0.0f, 0.0f});
    EXPECT_FALSE(fs::exists(AutosaveFile())) << "a drag in progress isn't saved";
    EndGroup();
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    EXPECT_EQ(json::parse(ReadFile(AutosaveFile())), SceneJson()) << "the scene as it is, with the drag's last position";
    EXPECT_EQ(Entity(a).at("transform").at("position").at("x").get<float>(), 2.0f);
}

TEST_F(EditorHistoryProjectTest, UndoAndRedoUpdateTheAutosaveAndUndoingToTheSavedStateRemovesIt)
{
    Open("res://scenes/Main.scene");
    Create("A");
    Create("B");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    Undo();
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"A"}));
    Undo();
    EXPECT_FALSE(fs::exists(AutosaveFile())) << "nothing unsaved: an autosave would be out of date";
    Redo();
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"A"}));
}

TEST_F(EditorHistoryProjectTest, SavingRemovesTheAutosave)
{
    Open("res://scenes/Main.scene");
    Create("A");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    Save();
    EXPECT_FALSE(fs::exists(AutosaveFile()));
    EXPECT_FALSE(AutosaveInfo().at("exists").get<bool>());
    EXPECT_EQ(RootNames(ReadFile(SceneFile())), (std::vector<std::string>{"A"}));

    // And the next edit writes it again
    Create("B");
    EXPECT_TRUE(fs::is_regular_file(AutosaveFile()));
}

TEST_F(EditorHistoryProjectTest, SavingAsAnotherFileRemovesTheOldScenesAutosave)
{
    Open("res://scenes/Main.scene");
    Create("A");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    Save("res://scenes/Copy.scene");
    EXPECT_FALSE(fs::exists(AutosaveFile()));
    EXPECT_FALSE(fs::exists(_root / ".n2" / "autosave" / "scenes" / "Copy.scene"));
    Create("B");
    EXPECT_TRUE(fs::is_regular_file(_root / ".n2" / "autosave" / "scenes" / "Copy.scene")) << "named after its new file";
    EXPECT_FALSE(fs::exists(AutosaveFile()));
}

TEST_F(EditorHistoryProjectTest, ASceneWithoutAFileIsAutosavedUnderAFixedName)
{
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Scratch"})).type, SceneInfoType);
    Create("A");
    EXPECT_TRUE(fs::is_regular_file(_root / ".n2" / "autosave" / ".untitled.scene"));
    EXPECT_EQ(fs::path(AutosaveInfo().at("path").get<std::string>()), _root / ".n2" / "autosave" / ".untitled.scene");
}

TEST_F(EditorHistoryProjectTest, NothingIsAutosavedForAnUnchangedScene)
{
    Open("res://scenes/Main.scene");
    EXPECT_FALSE(fs::exists(_root / ".n2" / "autosave"));
    Create("A");
    Undo();
    EXPECT_FALSE(fs::exists(AutosaveFile()));
}

TEST_F(EditorHistoryProjectTest, AnAutosaveWaitsForItsIntervalAndTheHostsLoopWritesTheLastOne)
{
    // Time is the test's: nothing sleeps
    auto now = std::chrono::steady_clock::time_point{} + 1000s;
    server.SetAutosaveClock([&now] { return now; });
    server.SetAutosaveInterval(2s);
    Open("res://scenes/Main.scene");
    Create("First");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile())) << "the first is written at once";
    now += 1s;
    Create("Second");
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"First"})) << "the second waits";
    server.ProcessCommands();
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"First"})) << "the interval isn't over";

    now += 2s;
    server.ProcessCommands();
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"First", "Second"}))
        << "the loop writes what is due";
}

TEST_F(EditorHistoryProjectTest, AnAutosaveFromBeforeIsKeptUntilItIsRestoredDiscardedOrTheSceneSaved)
{
    // What a crash left: an autosave of the scene with an object the file doesn't have
    const std::string recovered = R"({"name":"Main","rootGameObjects":[)"
                                  R"({"uuid":"123e4567-e89b-12d3-a456-426614174000","name":"Recovered","tag":"Untagged",)"
                                  R"("layer":0,"isActive":true,"components":[],"children":[]}]})";
    WriteFile(AutosaveFile(), recovered);

    Open("res://scenes/Main.scene");
    EXPECT_TRUE(server.IsAutosaveProtected());
    EXPECT_TRUE(AutosaveInfo().at("exists").get<bool>()) << "a client finds it and offers it";

    Create("New Work");
    EXPECT_EQ(ReadFile(AutosaveFile()), recovered) << "not overwritten before the client has decided";
    Undo();
    EXPECT_EQ(ReadFile(AutosaveFile()), recovered) << "nor removed, though the scene is back at its saved state";

    // Restoring replaces the scene's content, as a step that can be undone
    const uint32_t frameBeforeRestore = server.GetFrameRevision();
    const SceneInfoData restored = DecodeSceneInfo(Execute(server, CommandType::RestoreAutosave));
    EXPECT_NE(server.GetFrameRevision(), frameBeforeRestore) << "the viewport shows another scene";
    EXPECT_NE(restored.revision, restored.savedRevision) << "it differs from the file";
    EXPECT_FALSE(server.IsAutosaveProtected());
    ASSERT_TRUE(SceneManager::GetCurSceneRef().FindGameObject("Recovered") != nullptr);
    EXPECT_EQ(Labels().back(), "Restore autosave");
    EXPECT_TRUE(LastEventOfKind(server, "sceneChanged").value("full", false));

    EXPECT_EQ(Undo().label, "Restore autosave");
    EXPECT_TRUE(SceneManager::GetCurSceneRef().FindGameObject("Recovered") == nullptr);
    EXPECT_EQ(Redo().label, "Restore autosave");
    EXPECT_TRUE(SceneManager::GetCurSceneRef().FindGameObject("Recovered") != nullptr);
    // From now on it is written as usual
    Create("After");
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"Recovered", "After"}));
}

TEST_F(EditorHistoryProjectTest, DiscardingAnAutosaveRemovesItAndLetsTheHostWriteNewOnes)
{
    WriteFile(AutosaveFile(), R"({"name":"Main","rootGameObjects":[]})");
    Open("res://scenes/Main.scene");
    ASSERT_TRUE(server.IsAutosaveProtected());

    EXPECT_EQ(Execute(server, CommandType::DiscardAutosave).type, OkType);
    EXPECT_FALSE(fs::exists(AutosaveFile()));
    EXPECT_FALSE(server.IsAutosaveProtected());
    EXPECT_EQ(Execute(server, CommandType::DiscardAutosave).type, OkType) << "none is not an error";

    Create("A");
    EXPECT_TRUE(fs::is_regular_file(AutosaveFile()));
}

TEST_F(EditorHistoryProjectTest, SavingEndsTheProtectionOfAnOldAutosave)
{
    WriteFile(AutosaveFile(), R"({"name":"Main","rootGameObjects":[]})");
    Open("res://scenes/Main.scene");
    ASSERT_TRUE(server.IsAutosaveProtected());
    Create("A");
    Save();
    EXPECT_FALSE(server.IsAutosaveProtected());
    EXPECT_FALSE(fs::exists(AutosaveFile()));
}

TEST_F(EditorHistoryProjectTest, RestoringAnAutosaveThatIsntASceneIsAnErrorAndChangesNothing)
{
    WriteFile(AutosaveFile(), "{ not json");
    Open("res://scenes/Main.scene");
    Create("A");
    const json before = SceneJson();
    const uint32_t revision = OpenSceneInfo().revision;

    ExpectError(Execute(server, CommandType::RestoreAutosave), "can't be used");
    EXPECT_EQ(SceneJson(), before);
    EXPECT_EQ(OpenSceneInfo().revision, revision);
    EXPECT_EQ(ReadFile(AutosaveFile()), "{ not json") << "kept";
    EXPECT_EQ(Labels(), (std::vector<std::string>{"Create A"}));
}

TEST_F(EditorHistoryProjectTest, RestoringWithoutAnAutosaveIsAnError)
{
    Open("res://scenes/Main.scene");
    ExpectError(Execute(server, CommandType::RestoreAutosave), "no autosave");
}

TEST_F(EditorHistoryProjectTest, AutosaveCommandsNeedAProjectAndAScene)
{
    // A server of its own, without a project
    EditorServer bare;
    const Frame noProject = Execute(bare, CommandType::GetAutosave);
    EXPECT_EQ(noProject.type, ErrorType);
    EXPECT_NE(noProject.Text().find("No project"), std::string::npos);
    EXPECT_EQ(Execute(bare, CommandType::RestoreAutosave).type, ErrorType);
    EXPECT_EQ(Execute(bare, CommandType::DiscardAutosave).type, ErrorType);
    EXPECT_TRUE(bare.GetAutosaveFile().empty());
}

TEST_F(EditorHistoryProjectTest, AServerWithoutAProjectWritesNoAutosave)
{
    EditorServer bare;
    ASSERT_EQ(Execute(bare, CommandType::NewScene, Strings({"", "No Project"})).type, SceneInfoType);
    bare.SetAutosaveInterval(0ms);
    BufferWriter w;
    w.WriteString("A");
    w.WriteString("");
    w.WriteI32(-1);
    w.WriteString("");
    EXPECT_EQ(Execute(bare, CommandType::CreateEntityEx, w.Release()).type, EntityCreatedType);
    EXPECT_FALSE(fs::exists(_root / ".n2" / "autosave"));
    EXPECT_EQ(Execute(bare, CommandType::Undo).type, EditResultType);
    EXPECT_TRUE(bare.GetHistory().CanRedo());
}

// ==================== Review fixes: edits ====================

TEST_F(EditorHistoryTest, TheSameLegacyTransformAgainMovesNeitherTheRevisionNorTheHistory)
{
    const std::string entity = Create("Still");
    const uint32_t revision = OpenSceneInfo().revision;
    const size_t steps = History().entries.size();
    BufferWriter w;
    w.WriteString(entity);
    for (const float v : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f})
        w.WriteF32(v);
    EXPECT_EQ(Execute(server, CommandType::SetEntityTransform, w.Release()).type, OkType);
    EXPECT_EQ(OpenSceneInfo().revision, revision);
    EXPECT_EQ(History().entries.size(), steps);
}

TEST_F(EditorHistoryTest, UndoingAnActiveFlagTellsTheComponent)
{
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    const GameObject *object = SceneManager::GetCurSceneRef().FindGameObject("Host").get();
    ASSERT_NE(object, nullptr);
    ASSERT_EQ(SetFields(entity, component, json{{"isActive", false}}).type, ComponentDataType);
    EXPECT_EQ(object->GetComponent<HistoryHolder>()->activeChanges, 1);

    Undo();
    EXPECT_TRUE(Get(entity, component).at("isActive").get<bool>());
    EXPECT_EQ(object->GetComponent<HistoryHolder>()->activeChanges, 2) << "OnActiveFlagChanged ran for the undo";
    Redo();
    EXPECT_FALSE(Get(entity, component).at("isActive").get<bool>());
    EXPECT_EQ(object->GetComponent<HistoryHolder>()->activeChanges, 3);
}

TEST_F(EditorHistoryTest, ReparentingWithoutKeepingTheWorldTransformIsUndone)
{
    const std::string parent = Create("Parent");
    ASSERT_EQ(SetLocal(parent, {10.0f, 0.0f, 0.0f}).type, OkType);
    const std::string child = Create("Child");
    ASSERT_EQ(SetLocal(child, {1.0f, 1.0f, 1.0f}).type, OkType);
    RoundTrip("Reparent Child", [&] { EXPECT_EQ(Reparent(child, parent, 0, false).type, OkType); });
    EXPECT_EQ(Entity(child).at("transform").at("position").at("x").get<float>(), 1.0f) << "the local transform is kept";
}

TEST_F(EditorHistoryTest, ReparentingAnObjectWithoutATransformIsUndone)
{
    const std::string parent = Create("Parent");
    const Frame made = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    ASSERT_EQ(made.type, EntityCreatedType);
    BufferReader r(made.payload);
    const std::string bare = r.ReadString();
    RoundTrip("Reparent Bare", [&] { EXPECT_EQ(Reparent(bare, parent, 0).type, OkType); });
    EXPECT_FALSE(Entity(bare).contains("transform"));
}

TEST_F(EditorHistoryTest, ADeleteGroupThatWouldHoldTooMuchIsRefusedAndWhatDidFitUndoes)
{
    std::vector<std::string> ids;
    for (int i = 0; i < 20; ++i)
        ids.push_back(Create("Object " + std::to_string(i)));
    const json before = SceneJson();
    const size_t snapshot = before.dump().size();
    server.GetHistory().SetLimits(200, snapshot * 2);

    ASSERT_EQ(BeginGroup("Delete many").type, OkType);
    EXPECT_EQ(Destroy(ids[0]).type, OkType);
    EXPECT_EQ(Destroy(ids[1]).type, OkType);
    ExpectError(Destroy(ids[2]), "too much to undo");
    EXPECT_TRUE(Exists(ids[2])) << "a refused delete changes nothing";
    EXPECT_LE(server.GetHistory().GroupBytes(), snapshot * 3);
    ASSERT_EQ(EndGroup().type, OkType);

    EXPECT_EQ(Undo().label, "Delete many");
    EXPECT_EQ(SceneJson(), before);
}

TEST_F(EditorHistoryTest, ARemoveComponentInAGroupHoldsTheSameGuard)
{
    const std::string entity = Create("Host");
    const std::string first = AddHolder(entity);
    const std::string second = AddHolder(entity);
    server.GetHistory().SetLimits(200, 1);
    BeginGroup("Remove both");
    EXPECT_EQ(RemoveComponent(entity, first).type, OkType) << "the first always fits an empty group";
    ExpectError(RemoveComponent(entity, second), "too much to undo");
    EndGroup();
}

TEST_F(EditorHistoryTest, JsonHeldByAStepIsCountedAtItsTextSize)
{
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    ASSERT_EQ(SetFields(entity, component, json{{"label", std::string(2000, 'x')}}).type, ComponentDataType);
    const size_t text = Get(entity, component).dump().size();
    EXPECT_GE(History().entries.back().at("bytes").get<size_t>(), text + 100) << "the after, and the before, held as text";
}

TEST_F(EditorHistoryTest, AStepThatWasUndoneAndRedoneIsNotCoalescedInto)
{
    server.GetHistory().SetCoalesceWindow(1h);
    const std::string entity = Create("Host");
    const std::string component = AddHolder(entity);
    ASSERT_EQ(SetFields(entity, component, json{{"count", 2}}).type, ComponentDataType);
    Undo();
    Redo();
    const size_t steps = History().entries.size();
    ASSERT_EQ(SetFields(entity, component, json{{"count", 3}}).type, ComponentDataType);
    EXPECT_EQ(History().entries.size(), steps + 1);
}

TEST_F(EditorHistoryTest, ClosingEditGroupsEndsAGroupAClientLeftOpenAndSaysSo)
{
    const std::string a = Create("A");
    BeginGroup("Abandoned");
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    const size_t events = EventsOfKind(server, "historyChanged").size();
    server.CloseEditGroups(); // what the host queues when the connection closes
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(EventsOfKind(server, "historyChanged").size(), events + 1);
    EXPECT_EQ(Undo().label, "Abandoned");
    server.CloseEditGroups(); // none open: nothing happens
}

TEST_F(EditorHistoryTest, ASceneThatIsNotInEditModeRecordsNothingAndRefusesUndo)
{
    const std::string a = Create("A");
    ASSERT_EQ(History().entries.size(), 1u);
    SceneManager::GetCurSceneRef().SetEditMode(false);
    Create("Played");
    EXPECT_EQ(History().entries.size(), 1u) << "no step for a scene that runs";
    ExpectError(Execute(server, CommandType::Undo), "opened for editing");
    ExpectError(Execute(server, CommandType::Redo), "opened for editing");
    SceneManager::GetCurSceneRef().SetEditMode(true);
    EXPECT_EQ(Undo().label, "Create A");
    (void)a;
}

// ==================== Review fixes: with a project ====================

TEST_F(EditorHistoryProjectTest, ASaveInsideAnOpenGroupIsNotTheStateUndoGoesBackTo)
{
    Open("res://scenes/Main.scene");
    BeginGroup("Drag");
    const std::string a = Create("A");
    const SceneInfoData saved = Save();
    EXPECT_EQ(saved.revision, saved.savedRevision);
    SetLocal(a, {1.0f, 0.0f, 0.0f});
    EndGroup();
    EXPECT_NE(OpenSceneInfo().revision, OpenSceneInfo().savedRevision);

    const EditResultData undone = Undo();
    EXPECT_NE(undone.revision, undone.savedRevision) << "the file has A; the scene doesn't";
    const EditResultData redone = Redo();
    EXPECT_NE(redone.revision, redone.savedRevision) << "the file has A at the origin";
}

TEST_F(EditorHistoryProjectTest, UndoingATransformGivenToAnObjectWithoutOneLeavesTheSceneUnsaved)
{
    Open("res://scenes/Main.scene");
    const Frame made = Execute(server, CommandType::CreateEntity, Strings({"Bare"}));
    ASSERT_EQ(made.type, EntityCreatedType);
    BufferReader r(made.payload);
    const std::string bare = r.ReadString();
    const SceneInfoData saved = Save();
    ASSERT_EQ(saved.revision, saved.savedRevision);

    ASSERT_EQ(SetLocal(bare, {1.0f, 0.0f, 0.0f}).type, OkType);
    const EditResultData undone = Undo();
    // The object keeps the transform it was given (the identity): the scene saves a positionable the file doesn't have
    EXPECT_NE(undone.revision, undone.savedRevision);
}

TEST_F(EditorHistoryProjectTest, LeavingASceneRemovesTheAutosaveThisHostWroteForIt)
{
    Open("res://scenes/Main.scene");
    Create("A");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));

    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Other"})).type, SceneInfoType);
    EXPECT_FALSE(fs::exists(AutosaveFile())) << "the changes were dropped with the scene";

    // Opening the same scene again (reverting it) too
    Open("res://scenes/Main.scene");
    Create("B");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    Open("res://scenes/Main.scene");
    EXPECT_FALSE(fs::exists(AutosaveFile()));

    // LoadScene leaves the scene as well
    Create("C");
    ASSERT_TRUE(fs::is_regular_file(AutosaveFile()));
    ASSERT_EQ(Execute(server, CommandType::LoadScene, Strings({R"({"name":"Loaded","rootGameObjects":[]})"})).type, OkType);
    EXPECT_FALSE(fs::exists(AutosaveFile()));
}

TEST_F(EditorHistoryProjectTest, LeavingASceneKeepsAnAutosaveNobodyHasDecidedAbout)
{
    WriteFile(AutosaveFile(), R"({"name":"Main","rootGameObjects":[]})");
    Open("res://scenes/Main.scene");
    ASSERT_TRUE(server.IsAutosaveProtected());
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Other"})).type, SceneInfoType);
    EXPECT_TRUE(fs::is_regular_file(AutosaveFile())) << "what a crash left is still there for the next session";
}

TEST_F(EditorHistoryProjectTest, ASceneOpenedWithoutAnyoneHavingEditedItDoesntTouchAnotherSessionsAutosave)
{
    // An untitled scene's leftover, before this host has a scene at all
    const fs::path untitled = _root / ".n2" / "autosave" / ".untitled.scene";
    WriteFile(untitled, R"({"name":"Scratch","rootGameObjects":[]})");
    Open("res://scenes/Main.scene");
    EXPECT_TRUE(fs::is_regular_file(untitled));
}

TEST_F(EditorHistoryProjectTest, AnAutosaveOfAnotherSceneIsNotRestored)
{
    WriteFile(AutosaveFile(), R"({"name":"Somebody Else","rootGameObjects":[)"
              R"({"uuid":"123e4567-e89b-12d3-a456-426614174000","name":"Intruder","components":[],"children":[]}]})");
    Open("res://scenes/Main.scene");
    const json before = SceneJson();
    ExpectError(Execute(server, CommandType::RestoreAutosave), "Somebody Else");
    EXPECT_EQ(SceneJson(), before);
    EXPECT_TRUE(History().entries.empty());
}

TEST_F(EditorHistoryProjectTest, UndoingADestroyKeepsTheNameOfAFilelessScene)
{
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Named Scratch"})).type, SceneInfoType);
    const std::string a = Create("A");
    ASSERT_EQ(Destroy(a).type, OkType);
    Undo();
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "Named Scratch");
    EXPECT_EQ(OpenSceneInfo().name, "Named Scratch");
}

TEST_F(EditorHistoryProjectTest, AProtectedAutosaveIsMentionedOnceInTheLog)
{
    WriteFile(AutosaveFile(), R"({"name":"Main","rootGameObjects":[]})");
    Open("res://scenes/Main.scene");
    Create("A");
    Create("B");
    Create("C");
    size_t mentions = 0;
    for (const json &event : EventsOfKind(server, "log"))
    {
        if (event.value("message", "").find("is kept, and none is written over it") != std::string::npos)
            ++mentions;
    }
    EXPECT_EQ(mentions, 1u);
}

TEST_F(EditorHistoryProjectTest, TheAutosaveOfADestroyHoldsTheSceneWithTheObjectGone)
{
    Open("res://scenes/Main.scene");
    const std::string a = Create("A");
    const std::string b = Create("B");
    const std::string holder = AddHolder(b);
    ASSERT_EQ(SetFields(b, holder, json{{"target", a}}).type, ComponentDataType);
    ASSERT_EQ(Destroy(a).type, OkType);
    // Written after the purge, with the reference to the destroyed object cleared
    EXPECT_EQ(json::parse(ReadFile(AutosaveFile())), SceneJson());
    EXPECT_EQ(RootNames(ReadFile(AutosaveFile())), (std::vector<std::string>{"B"}));
}

// ==================== Review fixes: Lua ====================

namespace
{
    /// A project with a script of two fields, and the Lua runtime
    class EditorHistoryLuaTest : public EditorHistoryProjectTest
    {
    protected:
        void SetUp() override
        {
            EditorHistoryProjectTest::SetUp();
            ASSERT_TRUE(Scripting::LuaRuntime::Instance().Initialize());
            WriteFile(_root / "assets" / "scripts" / "Fields.lua", R"(
                local Fields = {}
                Fields.__index = Fields
                Fields.SerializableFields = {
                    count = 3,
                    label = { type = "string", default = "hi" },
                }
                return Fields
            )");
            ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
            Open("res://scenes/Main.scene");
        }

        std::string ScriptId() const
        {
            return IO::ResourceLoader::Instance().GetUUID(IO::ResourcePath("res://scripts/Fields.lua")).ToString();
        }

        std::string AddLua(const std::string &entity)
        {
            const Frame added = Execute(server, CommandType::AddComponent, Strings({entity, "LuaComponent"}));
            EXPECT_EQ(added.type, ComponentAddedType) << added.Text();
            BufferReader r(added.payload);
            return r.ReadString();
        }
    };
}

TEST_F(EditorHistoryLuaTest, AddingALuaComponentIsUndoneAndRedone)
{
    const std::string entity = Create("Scripted");
    std::string component;
    RoundTrip("Add LuaComponent", [&] { component = AddLua(entity); });
    EXPECT_EQ(Get(entity, component).at("uuid"), component);
}

TEST_F(EditorHistoryLuaTest, ChoosingTheScriptAndEditingItsDataAreUndoneAndRedone)
{
    const std::string entity = Create("Scripted");
    const std::string component = AddLua(entity);
    RoundTrip("Set scriptUUID", [&] { EXPECT_EQ(SetFields(entity, component, json{{"scriptUUID", ScriptId()}}).type, ComponentDataType); });
    EXPECT_EQ(Get(entity, component).at("scriptData").at("count"), 3);

    RoundTrip("Set count", [&]
    {
        EXPECT_EQ(SetFields(entity, component, json{{"scriptData", {{"count", 9}}}}).type, ComponentDataType);
    });
    EXPECT_EQ(Get(entity, component).at("scriptData").at("count"), 9);
    Undo();
    EXPECT_EQ(Get(entity, component).at("scriptData").at("count"), 3);
}

TEST_F(EditorHistoryLuaTest, TwoFieldsOfOneScriptAreTwoStepsAndOneFieldTypedIsOne)
{
    server.GetHistory().SetCoalesceWindow(1h);
    const std::string entity = Create("Scripted");
    const std::string component = AddLua(entity);
    ASSERT_EQ(SetFields(entity, component, json{{"scriptUUID", ScriptId()}}).type, ComponentDataType);
    const size_t steps = History().entries.size();

    ASSERT_EQ(SetFields(entity, component, json{{"scriptData", {{"count", 4}}}}).type, ComponentDataType);
    ASSERT_EQ(SetFields(entity, component, json{{"scriptData", {{"count", 5}}}}).type, ComponentDataType);
    EXPECT_EQ(History().entries.size(), steps + 1) << "the same field again is one step";
    EXPECT_EQ(Labels().back(), "Set count");

    ASSERT_EQ(SetFields(entity, component, json{{"scriptData", {{"label", "bye"}}}}).type, ComponentDataType);
    EXPECT_EQ(History().entries.size(), steps + 2) << "another field of the same script is another step";
    EXPECT_EQ(Labels().back(), "Set label");

    EXPECT_EQ(Undo().label, "Set label");
    EXPECT_EQ(Get(entity, component).at("scriptData").at("count"), 5);
    EXPECT_EQ(Undo().label, "Set count");
    EXPECT_EQ(Get(entity, component).at("scriptData").at("count"), 3);
}

TEST_F(EditorHistoryLuaTest, ASnapshotRestoreRebuildsAScriptedObjectWithItsData)
{
    const std::string entity = Create("Scripted");
    const std::string component = AddLua(entity);
    ASSERT_EQ(SetFields(entity, component, json{{"scriptUUID", ScriptId()}}).type, ComponentDataType);
    ASSERT_EQ(SetFields(entity, component, json{{"scriptData", {{"count", 8}, {"label", "kept"}}}}).type, ComponentDataType);
    const json before = SceneJson();

    ASSERT_EQ(Destroy(entity).type, OkType);
    EXPECT_EQ(Undo().label, "Delete Scripted");
    EXPECT_EQ(SceneJson(), before);
    EXPECT_EQ(Get(entity, component).at("scriptData").at("count"), 8);
    EXPECT_EQ(Get(entity, component).at("scriptData").at("label"), "kept");

    // And the component removed and put back
    ASSERT_EQ(RemoveComponent(entity, component).type, OkType);
    Undo();
    EXPECT_EQ(SceneJson(), before);
    Redo();
    Undo();
    EXPECT_EQ(Get(entity, component).at("scriptUUID"), ScriptId());
}
