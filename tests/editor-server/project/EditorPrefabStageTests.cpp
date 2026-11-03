#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Positionable.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// The prefab stage (protocol 1.26.0, orchestrator/proposals/prefab-stage.md): OpenPrefabStage, SetPrefabStageMode,
// SavePrefabStage, ClosePrefabStage and GetPrefabStage through EditorServer::ExecuteCommand, against a project made by
// IO::CreateProject. Part of the editor_project_tests program, which owns ResourceLoader and SceneManager for its tests.
// No renderer here: the dimmed backdrop is the renderer tests' (StageDim*), the render path is the host smoke's.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t HierarchyType = static_cast<uint8_t>(ResponseType::Hierarchy);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t AssetCreatedType = static_cast<uint8_t>(ResponseType::AssetCreated);
    constexpr uint8_t StageInfoType = static_cast<uint8_t>(ResponseType::PrefabStageInfo);
    constexpr uint8_t HistoryType = static_cast<uint8_t>(ResponseType::History);

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

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    /// A component with a reference to a GameObject, to see what the stage does with one that points outside the prefab
    class StageTestHolder final : public SerializableComponent
    {
    public:
        explicit StageTestHolder(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterGameObjectRef("target", target);
        }

        [[nodiscard]] std::string GetTypeName() const override { return "EditorPrefabStageTest_Holder"; }

        GameObject *target = nullptr;
    };

    class EditorPrefabStageTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ComponentRegistry::Instance().Register(
                "EditorPrefabStageTest_Holder",
                [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<StageTestHolder>(gameObject); });

            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-prefab-stage-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            // A scene with a file, so its autosave has a path of its own
            const Frame made = Execute(server, CommandType::NewScene, Strings({"res://scenes/Stage Test.scene", "Stage Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            server.SetAutosaveInterval(std::chrono::milliseconds(0));
        }

        void TearDown() override
        {
            server.DiscardPrefabStage("test teardown");
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }
        [[nodiscard]] fs::path SceneFile() const { return Assets() / "scenes" / "Stage Test.scene"; }
        [[nodiscard]] fs::path SceneAutosave() const { return _root / ".n2" / "autosave" / "scenes" / "Stage Test.scene"; }

        std::string Create(const std::string &name, const std::string &parentId = "", const std::string &preset = "Empty")
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString(parentId);
            w.WriteI32(Last);
            w.WriteString(preset);
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        /// CreatePrefab's answer: the prefab asset's UUID
        std::string CreatePrefab(const std::string &entityId, const std::string &path)
        {
            BufferWriter w;
            w.WriteString(entityId);
            w.WriteString(path);
            w.WriteBool(false);
            const Frame frame = Execute(server, CommandType::CreatePrefab, w.Release());
            EXPECT_EQ(frame.type, AssetCreatedType) << frame.Text();
            if (frame.type != AssetCreatedType)
                return {};
            BufferReader r(frame.payload);
            (void)r.ReadString();
            return r.ReadString();
        }

        /// A prefab "Crate" with a child "Lid", made from scene objects (which then are an instance of it)
        std::string MakeCratePrefab()
        {
            _crate = Create("Crate");
            _lid = Create("Lid", _crate);
            return CreatePrefab(_crate, "res://Crate.prefab");
        }

        json Info(const Frame &frame)
        {
            EXPECT_EQ(frame.type, StageInfoType) << frame.Text();
            if (frame.type != StageInfoType)
                return json::object();
            BufferReader r(frame.payload);
            json info = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return info;
        }

        Frame OpenFrame(const std::string &uuidOrPath, const std::string &mode = "isolated", const std::string &anchor = "")
        {
            return Execute(server, CommandType::OpenPrefabStage, Strings({uuidOrPath, mode, anchor}));
        }

        json Open(const std::string &uuidOrPath, const std::string &mode = "isolated", const std::string &anchor = "")
        {
            return Info(OpenFrame(uuidOrPath, mode, anchor));
        }

        Frame CloseFrame(const bool discard)
        {
            BufferWriter w;
            w.WriteBool(discard);
            return Execute(server, CommandType::ClosePrefabStage, w.Release());
        }

        Frame Save() { return Execute(server, CommandType::SavePrefabStage); }
        json Stage() { return Info(Execute(server, CommandType::GetPrefabStage)); }
        json SetMode(const std::string &mode) { return Info(Execute(server, CommandType::SetPrefabStageMode, Strings({mode}))); }

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

        Frame SetLocalPosition(const std::string &id, const float x, const float y, const float z)
        {
            BufferWriter w;
            w.WriteString(id);
            for (const float v : {x, y, z, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f})
                w.WriteF32(v);
            return Execute(server, CommandType::SetLocalTransform, w.Release());
        }

        Frame Rename(const std::string &id, const std::string &name)
        {
            BufferWriter w;
            w.WriteString(id);
            w.WriteString(json{{"name", name}}.dump());
            return Execute(server, CommandType::SetEntityProperties, w.Release());
        }

        std::size_t UndoCount()
        {
            const Frame frame = Execute(server, CommandType::GetHistory);
            EXPECT_EQ(frame.type, HistoryType) << frame.Text();
            BufferReader r(frame.payload);
            const uint32_t cursor = r.ReadU32();
            (void)ReadJson(r);
            return cursor;
        }

        [[nodiscard]] std::string SceneJson() const
        {
            return SceneManager::GetCurSceneRef().Serialize().dump();
        }

        std::vector<json> Events(const std::string &kind)
        {
            std::vector<json> found;
            for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
            {
                if (event.at("kind") == kind)
                    found.push_back(event);
            }
            return found;
        }

        void Rescan()
        {
            ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
        }

        EditorServer server;
        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        std::string _crate;
        std::string _lid;
    };
}

// ==================== Open and close ====================

TEST_F(EditorPrefabStageTest, OpeningShowsThePrefabAloneWithTheFilesUuidsAndClosingPutsTheSceneBackAsItWas)
{
    const std::string uuid = MakeCratePrefab();
    ASSERT_FALSE(uuid.empty());
    (void)Create("Unsaved");
    const std::string sceneBefore = SceneJson();
    const std::size_t undoBefore = UndoCount();
    const uint32_t savedBefore = server.GetSavedRevision();
    const bool dirtyBefore = server.GetSceneRevision() != server.GetSavedRevision();
    ASSERT_TRUE(dirtyBefore);

    const json info = Open("res://Crate.prefab");
    EXPECT_EQ(info.at("open"), true);
    EXPECT_EQ(info.at("prefabUuid"), uuid);
    EXPECT_EQ(info.at("path"), "res://Crate.prefab");
    EXPECT_EQ(info.at("name"), "Crate");
    EXPECT_EQ(info.at("mode"), "isolated");
    EXPECT_EQ(info.at("dirty"), false);
    EXPECT_EQ(info.at("contextAvailable"), true) << "the source object is an instance";
    EXPECT_EQ(info.at("anchorEntityId"), _crate);
    EXPECT_EQ(info.at("sceneName"), "Stage Test");
    EXPECT_EQ(info.at("scenePath"), "res://scenes/Stage Test.scene");
    EXPECT_TRUE(info.at("warnings").empty()) << info.dump();
    EXPECT_TRUE(server.IsPrefabStageOpen());

    // The hierarchy is the prefab's, with the file's UUIDs (here the ones the scene objects have too)
    const json nodes = Nodes();
    ASSERT_EQ(nodes.size(), 2u) << nodes.dump();
    EXPECT_EQ(nodes[0].at("name"), "Crate");
    EXPECT_EQ(nodes[0].at("id"), _crate);
    EXPECT_FALSE(nodes[0].contains("prefab")) << "the file's root carries no link to itself";
    EXPECT_EQ(nodes[1].at("name"), "Lid");
    EXPECT_EQ(nodes[1].at("id"), _lid);
    EXPECT_EQ(UndoCount(), 0u) << "the stage has a history of its own";

    // An edit of the stage: its own undo step, the stage dirty, the scene untouched
    ASSERT_EQ(Rename(_lid, "Top").type, OkType);
    EXPECT_EQ(Stage().at("dirty"), true);
    EXPECT_EQ(UndoCount(), 1u);

    const Frame closed = CloseFrame(true);
    EXPECT_EQ(Info(closed).at("open"), false);
    EXPECT_FALSE(server.IsPrefabStageOpen());
    EXPECT_EQ(SceneJson(), sceneBefore) << "the parked scene comes back unchanged";
    EXPECT_EQ(UndoCount(), undoBefore) << "the scene's history is back";
    EXPECT_NE(server.GetSceneRevision(), server.GetSavedRevision()) << "still unsaved, as it was";
    EXPECT_EQ(server.GetSavedRevision(), savedBefore);
    EXPECT_EQ(ReadFile(Assets() / "Crate.prefab").find("Top"), std::string::npos) << "discarded: the file is untouched";
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate", "Lid", "Unsaved"}));
}

TEST_F(EditorPrefabStageTest, OpeningAndClosingPushTheEventsAClientRefetchesFrom)
{
    (void)MakeCratePrefab();
    (void)server.GetEvents().Read(0, 4096, 0);
    const std::size_t scenesBefore = Events("sceneChanged").size();
    const std::size_t stagesBefore = Events("prefabStageChanged").size();

    (void)Open("res://Crate.prefab");
    std::vector<json> stages = Events("prefabStageChanged");
    ASSERT_EQ(stages.size(), stagesBefore + 1);
    EXPECT_EQ(stages.back().at("stage").at("open"), true);
    const std::vector<json> scenes = Events("sceneChanged");
    ASSERT_GT(scenes.size(), scenesBefore);
    EXPECT_EQ(scenes.back().at("full"), true);
    EXPECT_EQ(scenes.back().at("path"), "") << "the loaded scene is the stage, which has no scene file";

    // The first edit makes it dirty (one event), the next edit does not say so again
    (void)Create("Extra", _crate);
    (void)Create("Extra2", _crate);
    stages = Events("prefabStageChanged");
    ASSERT_EQ(stages.size(), stagesBefore + 2);
    EXPECT_EQ(stages.back().at("stage").at("dirty"), true);

    ASSERT_EQ(CloseFrame(true).type, StageInfoType);
    stages = Events("prefabStageChanged");
    EXPECT_EQ(stages.back().at("stage").at("open"), false);
    EXPECT_EQ(Events("sceneChanged").back().at("full"), true);
}

TEST_F(EditorPrefabStageTest, OpenIsRefusedForWhatIsNotAPrefabAndChangesNothing)
{
    (void)MakeCratePrefab();
    const std::string sceneBefore = SceneJson();
    EXPECT_EQ(OpenFrame("res://Missing.prefab").type, ErrorType);
    EXPECT_EQ(OpenFrame("res://scenes/Stage Test.scene").type, ErrorType);
    EXPECT_EQ(OpenFrame(Math::UUID::Random().ToString()).type, ErrorType);
    EXPECT_EQ(OpenFrame("res://Crate.prefab", "sideways").type, ErrorType);
    EXPECT_EQ(OpenFrame("res://Crate.prefab", "context", _lid).type, ErrorType) << "the lid is not an instance";
    {
        std::ofstream(Assets() / "Broken.prefab", std::ios::binary) << R"({"name":"Broken"})";
    }
    Rescan();
    EXPECT_EQ(OpenFrame("res://Broken.prefab").type, ErrorType);
    EXPECT_FALSE(server.IsPrefabStageOpen());
    EXPECT_EQ(SceneJson(), sceneBefore);

    // An open edit group: the stage would take it over
    ASSERT_EQ(Execute(server, CommandType::BeginEditGroup, Strings({"Drag"})).type, OkType);
    EXPECT_EQ(OpenFrame("res://Crate.prefab").type, ErrorType);
    ASSERT_EQ(Execute(server, CommandType::EndEditGroup).type, OkType);
    EXPECT_FALSE(server.IsPrefabStageOpen());
}

// ==================== What a stage refuses ====================

TEST_F(EditorPrefabStageTest, EveryCommandThatSavesOpensSwapsOrPlaysTheSceneIsRefusedWithStageOpen)
{
    (void)MakeCratePrefab();
    const std::string sceneFileBefore = ReadFile(SceneFile());
    (void)Open("res://Crate.prefab");
    (void)Create("StageOnly", _crate);

    for (const CommandType type : {CommandType::SaveSceneToFile, CommandType::SaveScene, CommandType::OpenScene,
                                   CommandType::NewScene, CommandType::LoadScene, CommandType::CreateScene,
                                   CommandType::DeleteScene, CommandType::RestoreAutosave, CommandType::DiscardAutosave,
                                   CommandType::WritePlaySnapshot, CommandType::CreatePrefab,
                                   CommandType::InstantiatePrefab, CommandType::ApplyPrefab, CommandType::UnpackPrefab,
                                   CommandType::OpenPrefabStage})
    {
        EXPECT_TRUE(EditorServer::IsRefusedWhilePrefabStageOpen(static_cast<uint8_t>(type))) << static_cast<int>(type);
        const Frame refused = Execute(server, type);
        EXPECT_EQ(refused.type, ErrorType) << static_cast<int>(type);
        EXPECT_NE(refused.Text().find("A prefab stage is open"), std::string::npos) << refused.Text();
    }
    for (const CommandType type : {CommandType::SetStartupScene, CommandType::GetHierarchy, CommandType::Undo,
                                   CommandType::SerializeEntities, CommandType::InstantiateEntities,
                                   CommandType::RescanAssets, CommandType::SetEditorCamera})
    {
        EXPECT_FALSE(EditorServer::IsRefusedWhilePrefabStageOpen(static_cast<uint8_t>(type))) << static_cast<int>(type);
    }
    EXPECT_EQ(ReadFile(SceneFile()), sceneFileBefore) << "nothing of the stage reached the scene file";
    EXPECT_TRUE(server.IsPrefabStageOpen());

    // A client that asked for codes gets stageOpen
    BufferWriter hello;
    hello.WriteString("test");
    hello.WriteString(std::string(ProtocolVersion));
    hello.WriteString("");
    hello.WriteBool(true);
    ASSERT_EQ(Execute(server, CommandType::Hello, hello.Release()).type, static_cast<uint8_t>(ResponseType::ServerInfo));
    const Frame coded = Execute(server, CommandType::SaveScene);
    ASSERT_EQ(coded.type, ErrorCodedType);
    BufferReader r(coded.payload);
    (void)r.ReadString();
    EXPECT_EQ(r.ReadString(), ErrorCodeStageOpen);

    // Closed: the scene saves, without the stage's objects
    ASSERT_EQ(CloseFrame(true).type, StageInfoType);
    ASSERT_EQ(Execute(server, CommandType::SaveSceneToFile, Strings({""})).type, SceneInfoType);
    EXPECT_EQ(ReadFile(SceneFile()).find("StageOnly"), std::string::npos);
}

TEST_F(EditorPrefabStageTest, AnEditorExtensionsWritesAreRefusedAndItsReadsSeeTheStage)
{
    (void)MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    BufferWriter w;
    w.WriteString("FromExtension");
    w.WriteString("");
    w.WriteI32(Last);
    w.WriteString("Empty");
    const std::vector<uint8_t> refused = server.DispatchNestedForTest(static_cast<uint8_t>(CommandType::CreateEntityEx), w.Release());
    ASSERT_FALSE(refused.empty());
    EXPECT_EQ(refused.front(), ErrorType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate", "Lid"}));

    const std::vector<uint8_t> read = server.DispatchNestedForTest(static_cast<uint8_t>(CommandType::GetHierarchy), {});
    ASSERT_FALSE(read.empty());
    EXPECT_EQ(read.front(), HierarchyType);
}

TEST_F(EditorPrefabStageTest, TheScenesAutosaveWaitsWhileTheStageIsOpenAndComesBackAfter)
{
    (void)MakeCratePrefab();
    std::error_code error;
    ASSERT_TRUE(fs::exists(SceneAutosave(), error)) << "the scene's unsaved edits were autosaved";
    const std::string autosaveBefore = ReadFile(SceneAutosave());

    (void)Open("res://Crate.prefab");
    (void)Create("StageEdit", _crate);
    (void)Create("StageEdit2", _crate);
    EXPECT_EQ(ReadFile(SceneAutosave()), autosaveBefore) << "the stage writes no autosave, the scene's is left alone";
    EXPECT_FALSE(fs::exists(_root / ".n2" / "autosave" / ".untitled.scene", error));
    ASSERT_EQ(CloseFrame(true).type, StageInfoType);

    // After the stage the scene autosaves as before
    (void)Create("SceneEdit");
    EXPECT_NE(ReadFile(SceneAutosave()).find("SceneEdit"), std::string::npos);
    EXPECT_EQ(ReadFile(SceneAutosave()).find("StageEdit"), std::string::npos);
}

// ==================== Save ====================

TEST_F(EditorPrefabStageTest, SavingAnUntouchedStageWritesTheSameBytes)
{
    (void)MakeCratePrefab();
    const std::string fileBefore = ReadFile(Assets() / "Crate.prefab");
    (void)Open("res://Crate.prefab");
    const json saved = Info(Save());
    EXPECT_EQ(saved.at("dirty"), false);
    EXPECT_EQ(ReadFile(Assets() / "Crate.prefab"), fileBefore) << "the file's own UUIDs: no churn";
}

TEST_F(EditorPrefabStageTest, SaveWritesTheStageRootKeepsThePrefabsNameAndUndoBackToItIsClean)
{
    const std::string uuid = MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    ASSERT_EQ(Rename(_crate, "Crate Root").type, OkType);
    const std::string handle = Create("Handle", _crate);
    ASSERT_EQ(Stage().at("dirty"), true);

    const json saved = Info(Save());
    EXPECT_EQ(saved.at("dirty"), false);
    EXPECT_EQ(saved.at("savedRevision"), saved.at("revision"));
    const json file = json::parse(ReadFile(Assets() / "Crate.prefab"));
    EXPECT_EQ(file.at("name"), "Crate") << "the prefab's own name is kept";
    EXPECT_EQ(file.at("rootObject").at("name"), "Crate Root") << "the root object's is the stage's";
    EXPECT_EQ(file.at("rootObject").at("uuid"), _crate);
    EXPECT_FALSE(file.at("rootObject").contains("prefabReference"));
    EXPECT_NE(file.dump().find(handle), std::string::npos);

    // Undo after the save: unsaved; redo back to what was saved: clean again
    ASSERT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    EXPECT_EQ(Stage().at("dirty"), true);
    ASSERT_EQ(Execute(server, CommandType::Redo).type, EditResultType);
    EXPECT_EQ(Stage().at("dirty"), false);
    ASSERT_EQ(CloseFrame(false).type, StageInfoType) << "nothing unsaved";

    // The next instance is built from what was saved; the scene's instance is a copy and did not change
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate", "Lid"}));
    BufferWriter w;
    w.WriteString(uuid);
    w.WriteString("");
    w.WriteI32(Last);
    w.WriteBool(false);
    for (int i = 0; i < 3; ++i)
        w.WriteF32(0.0f);
    ASSERT_EQ(Execute(server, CommandType::InstantiatePrefab, w.Release()).type, EntityCreatedType);
    const std::vector<std::string> names = Names();
    EXPECT_NE(std::find(names.begin(), names.end(), "Crate Root"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "Handle"), names.end());
}

TEST_F(EditorPrefabStageTest, CloseWithUnsavedChangesIsRefusedUntilSavedOrDiscarded)
{
    (void)MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    (void)Create("Handle", _crate);

    const Frame refused = CloseFrame(false);
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("unsaved changes"), std::string::npos) << refused.Text();
    EXPECT_TRUE(server.IsPrefabStageOpen());

    ASSERT_EQ(Info(Save()).at("dirty"), false);
    EXPECT_EQ(Info(CloseFrame(false)).at("open"), false);
    EXPECT_NE(ReadFile(Assets() / "Crate.prefab").find("Handle"), std::string::npos);
}

TEST_F(EditorPrefabStageTest, SaveIsRefusedForAStageWithoutOneRootAndForAPrefabThatContainsItself)
{
    const std::string uuid = MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    (void)Create("Second Root");
    const Frame twoRoots = Save();
    EXPECT_EQ(twoRoots.type, ErrorType);
    EXPECT_NE(twoRoots.Text().find("exactly one root"), std::string::npos) << twoRoots.Text();
    ASSERT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    ASSERT_EQ(CloseFrame(true).type, StageInfoType);

    // A hand-made file with a child linked to the prefab itself
    const json file = json::parse(ReadFile(Assets() / "Crate.prefab"));
    json cyclic = file;
    cyclic["rootObject"]["children"][0]["prefabReference"] = uuid;
    {
        std::ofstream(Assets() / "Crate.prefab", std::ios::binary) << cyclic.dump(2);
    }
    Rescan();
    const json info = Open("res://Crate.prefab");
    EXPECT_FALSE(info.at("warnings").empty()) << "a linked object is edited as a plain one";
    const Frame cycle = Save();
    EXPECT_EQ(cycle.type, ErrorType);
    EXPECT_NE(cycle.Text().find("linked to the prefab itself"), std::string::npos) << cycle.Text();
}

TEST_F(EditorPrefabStageTest, AReferenceToAnObjectOutsideThePrefabIsClearedWithAWarningAndSavedEmpty)
{
    const std::string crate = Create("Crate");
    const std::string outside = Create("Outside");
    {
        // The holder on the crate points at an object of the scene, which the prefab does not contain
        BufferWriter add;
        add.WriteString(crate);
        add.WriteString("EditorPrefabStageTest_Holder");
        ASSERT_EQ(Execute(server, CommandType::AddComponent, add.Release()).type,
                  static_cast<uint8_t>(ResponseType::ComponentAdded));
    }
    Scene &scene = SceneManager::GetCurSceneRef();
    auto crateObject = scene.FindGameObjectByUUID(*Math::UUID::FromString(crate));
    auto outsideObject = scene.FindGameObjectByUUID(*Math::UUID::FromString(outside));
    ASSERT_NE(crateObject, nullptr);
    crateObject->GetComponent<StageTestHolder>()->target = outsideObject.get();
    (void)CreatePrefab(crate, "res://Crate.prefab");
    ASSERT_NE(ReadFile(Assets() / "Crate.prefab").find(outside), std::string::npos) << "CreatePrefab keeps the id";

    const json info = Open("res://Crate.prefab");
    ASSERT_EQ(info.at("warnings").size(), 1u) << info.dump();
    EXPECT_NE(info.at("warnings")[0].get<std::string>().find("1 reference to objects outside the prefab was cleared"),
              std::string::npos) << info.dump();
    (void)Info(Save());
    EXPECT_EQ(ReadFile(Assets() / "Crate.prefab").find(outside), std::string::npos) << "saved empty";
}

// ==================== Undo ====================

TEST_F(EditorPrefabStageTest, TheStagesUndoAndTheScenesUndoAreSeparate)
{
    (void)MakeCratePrefab();
    const std::size_t sceneSteps = UndoCount();
    ASSERT_GT(sceneSteps, 0u);
    (void)Open("res://Crate.prefab");
    EXPECT_EQ(UndoCount(), 0u);
    EXPECT_EQ(Execute(server, CommandType::Undo).type, ErrorType) << "nothing to undo in the stage";
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate", "Lid"}));

    (void)Create("Handle", _crate);
    ASSERT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate", "Lid"}));
    EXPECT_EQ(Stage().at("dirty"), false) << "undone back to the state it opened in";
    ASSERT_EQ(CloseFrame(false).type, StageInfoType);
    EXPECT_EQ(UndoCount(), sceneSteps);
}

TEST_F(EditorPrefabStageTest, UndoingADestroyInTheStageRestoresTheStageNotAStoredScene)
{
    (void)MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({_lid})).type, OkType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate"}));
    ASSERT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    EXPECT_EQ(Names(), (std::vector<std::string>{"Crate", "Lid"}));
    EXPECT_FALSE(SceneManager::HasScene("Crate")) << "the stage is never stored as a scene";
    ASSERT_EQ(CloseFrame(true).type, StageInfoType);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "Stage Test");
}

// ==================== In Scene (context) ====================

TEST_F(EditorPrefabStageTest, InSceneThePrefabSitsAtItsInstanceWithItsRootLockedAndSaveKeepsTheFilesTransform)
{
    (void)MakeCratePrefab();
    ASSERT_EQ(SetLocalPosition(_crate, 5.0f, 0.0f, 0.0f).type, OkType);
    const json fileBefore = json::parse(ReadFile(Assets() / "Crate.prefab"));

    json info = Open("res://Crate.prefab", "context");
    EXPECT_EQ(info.at("mode"), "context");
    EXPECT_EQ(info.at("rootLocked"), true);
    EXPECT_EQ(info.at("anchorEntityId"), _crate);
    EXPECT_EQ(info.at("dirty"), false);
    Scene &stage = SceneManager::GetCurSceneRef();
    auto root = stage.FindGameObjectByUUID(*Math::UUID::FromString(_crate));
    ASSERT_NE(root, nullptr);
    EXPECT_NEAR(root->GetPositionable()->GetPosition().x, 5.0f, 1e-5f) << "placed where the instance is";

    const Frame locked = SetLocalPosition(_crate, 1.0f, 2.0f, 3.0f);
    EXPECT_EQ(locked.type, ErrorType);
    EXPECT_NE(locked.Text().find("locked"), std::string::npos) << locked.Text();
    EXPECT_EQ(SetLocalPosition(_lid, 0.0f, 1.0f, 0.0f).type, OkType) << "children are editable";

    // Save writes the file's root transform, not the instance's place
    (void)Info(Save());
    const json saved = json::parse(ReadFile(Assets() / "Crate.prefab"));
    EXPECT_EQ(saved.at("rootObject").at("positionable"), fileBefore.at("rootObject").at("positionable"));

    // Isolated: the file's transform, editable; the mode switch is not an edit
    info = SetMode("isolated");
    EXPECT_EQ(info.at("dirty"), false);
    EXPECT_EQ(info.at("rootLocked"), false);
    root = SceneManager::GetCurSceneRef().FindGameObjectByUUID(*Math::UUID::FromString(_crate));
    EXPECT_NEAR(root->GetPositionable()->GetPosition().x, 0.0f, 1e-5f);
    ASSERT_EQ(SetLocalPosition(_crate, 0.0f, 0.0f, 7.0f).type, OkType);

    // Back in Scene: at the instance again; undoing the Isolated move changes what the file keeps, not the place
    info = SetMode("context");
    EXPECT_EQ(info.at("dirty"), true);
    root = SceneManager::GetCurSceneRef().FindGameObjectByUUID(*Math::UUID::FromString(_crate));
    EXPECT_NEAR(root->GetPositionable()->GetPosition().x, 5.0f, 1e-5f);
    ASSERT_EQ(Execute(server, CommandType::Undo).type, EditResultType);
    root = SceneManager::GetCurSceneRef().FindGameObjectByUUID(*Math::UUID::FromString(_crate));
    EXPECT_NEAR(root->GetPositionable()->GetPosition().x, 5.0f, 1e-5f) << "still at the instance";
    (void)Info(Save());
    EXPECT_EQ(json::parse(ReadFile(Assets() / "Crate.prefab")).at("rootObject").at("positionable"),
              fileBefore.at("rootObject").at("positionable"));
}

TEST_F(EditorPrefabStageTest, InSceneWithoutAnInstanceShowsThePrefabAtItsOwnPlace)
{
    (void)MakeCratePrefab();
    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({_crate})).type, OkType);
    const json info = Open("res://Crate.prefab", "context");
    EXPECT_EQ(info.at("contextAvailable"), false);
    EXPECT_FALSE(info.contains("anchorEntityId"));
    ASSERT_EQ(info.at("warnings").size(), 1u) << info.dump();
}

// ==================== The file changing under the stage ====================

TEST_F(EditorPrefabStageTest, AFileChangedByAnotherProgramIsReportedAndOurOwnSaveIsNot)
{
    (void)MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    (void)Info(Save());
    Rescan();
    EXPECT_EQ(Stage().at("externalChange"), false);

    json file = json::parse(ReadFile(Assets() / "Crate.prefab"));
    file["rootObject"]["name"] = "Changed Elsewhere";
    {
        std::ofstream(Assets() / "Crate.prefab", std::ios::binary) << file.dump(2);
    }
    Rescan();
    EXPECT_EQ(Stage().at("externalChange"), true);
    // Keep: a Save overwrites it and clears the flag
    EXPECT_EQ(Info(Save()).at("externalChange"), false);
}

TEST_F(EditorPrefabStageTest, AMovedFileIsFollowedAndADeletedOneCannotBeSaved)
{
    (void)MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    BufferWriter move;
    WriteJson(move, json::array({json{{"from", "res://Crate.prefab"}, {"to", "res://props/Crate.prefab"}}}));
    ASSERT_EQ(Execute(server, CommandType::MoveAsset, move.Release()).type, static_cast<uint8_t>(ResponseType::AssetsMoved));
    EXPECT_EQ(Stage().at("path"), "res://props/Crate.prefab");
    (void)Create("Handle", _crate);
    (void)Info(Save());
    EXPECT_NE(ReadFile(Assets() / "props" / "Crate.prefab").find("Handle"), std::string::npos);

    std::error_code error;
    fs::remove(Assets() / "props" / "Crate.prefab", error);
    Rescan();
    EXPECT_EQ(Stage().at("orphaned"), true);
    (void)Create("More", _crate);
    const Frame refused = Save();
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("deleted"), std::string::npos) << refused.Text();
    EXPECT_EQ(Info(CloseFrame(true)).at("open"), false);
}

TEST_F(EditorPrefabStageTest, MoveAssetStillProtectsTheParkedScenesFile)
{
    (void)MakeCratePrefab();
    (void)Open("res://Crate.prefab");
    BufferWriter move;
    WriteJson(move, json::array({json{{"from", "res://scenes/Stage Test.scene"}, {"to", "res://scenes/Other.scene"}}}));
    EXPECT_EQ(Execute(server, CommandType::MoveAsset, move.Release()).type, ErrorType);
}

// ==================== The session ====================

TEST_F(EditorPrefabStageTest, AShutdownOrAClientThatWentAwayClosesTheStageWithoutSaving)
{
    (void)MakeCratePrefab();
    const std::string sceneBefore = SceneJson();
    const std::string fileBefore = ReadFile(Assets() / "Crate.prefab");
    (void)Open("res://Crate.prefab");
    (void)Create("Handle", _crate);
    server.PrepareForShutdown();
    EXPECT_FALSE(server.IsPrefabStageOpen());
    EXPECT_EQ(SceneJson(), sceneBefore);
    EXPECT_EQ(ReadFile(Assets() / "Crate.prefab"), fileBefore);
    EXPECT_EQ(Stage().at("open"), false);
}

TEST_F(EditorPrefabStageTest, TheStageCommandsAnswerWithoutAStage)
{
    EXPECT_EQ(Stage().at("open"), false);
    EXPECT_EQ(Save().type, ErrorType);
    EXPECT_EQ(CloseFrame(true).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::SetPrefabStageMode, Strings({"context"})).type, ErrorType);
    const std::vector<std::string> capabilities = EditorServer::Capabilities();
    EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "prefabStage"), capabilities.end());
    EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "prefabStageContext"), capabilities.end());
}
