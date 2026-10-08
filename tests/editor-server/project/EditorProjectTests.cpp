#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Host.hpp>
#include <editor-server/HostOptions.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Layers.hpp>
#include <engine/Time.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// The project and scene-file commands (#75) through EditorServer::ExecuteCommand, against a real project folder made
// by IO::CreateProject. Its own executable: these tests load scenes into SceneManager and initialise ResourceLoader
// for their folder, process-wide state the other editor-server tests expect untouched.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t ProjectInfoType = static_cast<uint8_t>(ResponseType::ProjectInfo);

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

    std::vector<uint8_t> JsonPayload(const json &value)
    {
        BufferWriter w;
        WriteJson(w, value);
        return w.Release();
    }

    struct SceneInfo
    {
        std::string path;
        std::string name;
        std::string uuid;
        uint32_t revision = 0;
        uint32_t savedRevision = 0;
    };

    SceneInfo DecodeSceneInfo(const Frame &frame)
    {
        EXPECT_EQ(frame.type, SceneInfoType) << frame.Text();
        SceneInfo info;
        if (frame.type != SceneInfoType)
            return info;
        BufferReader r(frame.payload);
        info.path = r.ReadString();
        info.name = r.ReadString();
        info.uuid = r.ReadString();
        info.revision = r.ReadU32();
        info.savedRevision = r.ReadU32();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after SceneInfo";
        return info;
    }

    struct ProjectInfo
    {
        std::string rootPath;
        std::string userDataPath;
        json project;
    };

    ProjectInfo DecodeProjectInfo(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ProjectInfoType) << frame.Text();
        ProjectInfo info;
        if (frame.type != ProjectInfoType)
            return info;
        BufferReader r(frame.payload);
        info.rootPath = r.ReadString();
        info.userDataPath = r.ReadString();
        info.project = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after ProjectInfo";
        return info;
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    std::string Utf8(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
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

    bool SceneHasObjectNamed(const std::string &name)
    {
        const Scene *scene = SceneManager::GetCurScene();
        if (scene == nullptr)
            return false;
        for (const auto &object : SceneManager::GetCurSceneRef().GetAllGameObjects())
        {
            if (object->GetName() == name)
                return true;
        }
        return false;
    }

    /// A new project (IO::CreateProject) in a fresh temp folder, opened as RunHost opens one
    class EditorProjectTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-project-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }

        SceneInfo Open(const std::string &path)
        {
            return DecodeSceneInfo(Execute(server, CommandType::OpenScene, Strings({path})));
        }

        Frame Save(const std::string &path = "")
        {
            return Execute(server, CommandType::SaveSceneToFile, Strings({path}));
        }

        SceneInfo OpenSceneInfo()
        {
            return DecodeSceneInfo(Execute(server, CommandType::GetOpenScene));
        }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = Assets() / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

// ==================== OpenScene ====================

TEST_F(EditorProjectTest, OpenSceneLoadsTheFileWithItsAssetUuid)
{
    const SceneInfo info = Open("res://scenes/Main.scene");
    EXPECT_EQ(info.path, "res://scenes/Main.scene");
    EXPECT_EQ(info.name, "Main");
    EXPECT_EQ(info.uuid, IO::ResourceUUID::FromPath(IO::ResourcePath("res://scenes/Main.scene")).ToString());
    EXPECT_EQ(info.uuid, IO::ResourceLoader::Instance().GetUUID(IO::ResourcePath("res://scenes/Main.scene")).ToString())
        << "the scene's UUID is its file's .meta UUID";
    EXPECT_GT(info.revision, 0u);
    EXPECT_EQ(info.revision, info.savedRevision) << "a scene just opened has nothing unsaved";

    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_EQ(SceneManager::GetCurSceneRef().sceneName, "Main");
    EXPECT_EQ(SceneManager::GetCurSceneRef().GetUUID().ToString(), info.uuid);
}

TEST_F(EditorProjectTest, AScenesUuidIsTheSameEveryTimeItOpens)
{
    const SceneInfo first = Open("res://scenes/Main.scene");
    const SceneInfo second = Open("res://scenes/Main.scene");
    EXPECT_EQ(first.uuid, second.uuid);
    EXPECT_GT(second.revision, first.revision) << "opening again is another scene: the revision moves on";
}

TEST_F(EditorProjectTest, OpenSceneRefusesWhatIsntASceneFileOfTheProject)
{
    WriteAsset("scenes/NotJson.scene", "{ nope");
    WriteAsset("scenes/NotAScene.scene", R"({"rootGameObjects": 7})");
    WriteAsset("scenes/Main.json", R"({"name":"Json","rootGameObjects":[]})");
    const SceneInfo before = Open("res://scenes/Main.scene");

    for (const std::string path : {"", "scenes/Main.scene", "res://scenes/Main.json", "res://../outside.scene",
                                   "res://scenes/../../outside.scene", "user://Main.scene", "res://scenes/Missing.scene",
                                   "res://scenes/NotJson.scene", "res://scenes/NotAScene.scene",
                                   "C:/Windows/Main.scene", "/etc/Main.scene"})
    {
        const Frame response = Execute(server, CommandType::OpenScene, Strings({path}));
        EXPECT_EQ(response.type, ErrorType) << path;
    }

    // The loaded scene is untouched
    const SceneInfo after = OpenSceneInfo();
    EXPECT_EQ(after.path, before.path);
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_EQ(after.uuid, before.uuid);
}

#ifdef _WIN32
// Windows file names are case-insensitive: another spelling is the same file, so it must be the same scene
TEST_F(EditorProjectTest, AnotherSpellingOfTheSamePathIsTheSameScene)
{
    const SceneInfo canonical = Open("res://scenes/Main.scene");
    const SceneInfo variant = Open("res://SCENES/main.SCENE");
    EXPECT_EQ(variant.path, "res://scenes/Main.scene") << "the file system's spelling";
    EXPECT_EQ(variant.uuid, canonical.uuid);

    // A save through the other spelling is a save to the scene's own file, not a "save as"
    const SceneInfo saved = DecodeSceneInfo(Save("res://Scenes/MAIN.scene"));
    EXPECT_EQ(saved.path, "res://scenes/Main.scene");
    EXPECT_EQ(saved.uuid, canonical.uuid);
    EXPECT_FALSE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath("res://Scenes/MAIN.scene")))
        << "no second .meta for the other spelling";
}
#endif

TEST_F(EditorProjectTest, ANonAsciiSceneFileOpensUnderItsUtf8Path)
{
    // "Caf\xC3\xA9" is "Cafe" with an e-acute, as UTF-8 (the source stays ASCII)
    const std::string path = "res://scenes/Caf\xC3\xA9.scene";
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({path, ""})).type, SceneInfoType);
    EXPECT_TRUE(fs::exists(Assets() / "scenes" / IO::PathFromUtf8("Caf\xC3\xA9.scene")));
    EXPECT_TRUE(fs::exists(_root / ".import" / "scenes" / IO::PathFromUtf8("Caf\xC3\xA9.scene.meta")));

    const SceneInfo opened = Open(path);
    EXPECT_EQ(opened.path, path);
    EXPECT_EQ(opened.name, "Caf\xC3\xA9");
    EXPECT_EQ(opened.uuid, IO::ResourceUUID::FromPath(IO::ResourcePath(path)).ToString());
    EXPECT_TRUE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath(path)));
}

TEST_F(EditorProjectTest, ASymlinkedSceneFileThatLeavesTheProjectIsRefused)
{
    const fs::path outside = _base / "outside.scene";
    std::ofstream(outside, std::ios::binary) << R"({"name":"Outside","rootGameObjects":[]})";
    std::error_code error;
    fs::create_symlink(outside, Assets() / "scenes" / "Link.scene", error);
    if (error)
    {
        GTEST_SKIP() << "can't create a symlink here (" << error.message() << ")";
    }
    const std::string before = ReadFile(outside);

    const Frame opened = Execute(server, CommandType::OpenScene, Strings({"res://scenes/Link.scene"}));
    EXPECT_EQ(opened.type, ErrorType) << "a read through the link";
    Open("res://scenes/Main.scene");
    EXPECT_EQ(Save("res://scenes/Link.scene").type, ErrorType) << "a write through the link";
    EXPECT_EQ(ReadFile(outside), before);
}

TEST_F(EditorProjectTest, ASymlinkedFolderThatLeavesTheProjectIsRefused)
{
    const fs::path outside = _base / "outside-folder";
    fs::create_directories(outside);
    std::error_code error;
    fs::create_directory_symlink(outside, Assets() / "linked", error);
    if (error)
    {
        GTEST_SKIP() << "can't create a directory symlink here (" << error.message() << ")";
    }
    Open("res://scenes/Main.scene");
    EXPECT_EQ(Save("res://linked/Escaped.scene").type, ErrorType);
    EXPECT_FALSE(fs::exists(outside / "Escaped.scene"));
    EXPECT_EQ(Execute(server, CommandType::NewScene, Strings({"res://linked/New.scene", ""})).type, ErrorType);
    EXPECT_FALSE(fs::exists(outside / "New.scene"));
}

TEST_F(EditorProjectTest, AFileAddedSinceTheScanOpensAndIsIndexed)
{
    WriteAsset("levels/Late.scene", R"({"name":"Late","rootGameObjects":[]})");
    const SceneInfo info = Open("res://levels/Late.scene");
    EXPECT_EQ(info.name, "Late");
    EXPECT_TRUE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath("res://levels/Late.scene")));
}

// ==================== savedRevision and SaveSceneToFile ====================

TEST_F(EditorProjectTest, EditsLeaveTheSceneUnsavedUntilItIsSaved)
{
    const SceneInfo opened = Open("res://scenes/Main.scene");

    ASSERT_EQ(Execute(server, CommandType::CreateEntity, Strings({"Saved Box"})).type,
              static_cast<uint8_t>(ResponseType::EntityCreated));
    const SceneInfo edited = OpenSceneInfo();
    EXPECT_GT(edited.revision, opened.revision);
    EXPECT_EQ(edited.savedRevision, opened.savedRevision);
    EXPECT_NE(edited.revision, edited.savedRevision) << "unsaved";

    const SceneInfo saved = DecodeSceneInfo(Save());
    EXPECT_EQ(saved.path, "res://scenes/Main.scene");
    EXPECT_EQ(saved.revision, edited.revision) << "saving doesn't change the scene";
    EXPECT_EQ(saved.savedRevision, saved.revision) << "saved";
    EXPECT_EQ(saved.uuid, opened.uuid);

    // The file holds the scene, indented, ending in a newline
    const std::string text = ReadFile(Assets() / "scenes" / "Main.scene");
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');
    EXPECT_NE(text.find("\n  \"name\": \"Main\""), std::string::npos) << text;
    EXPECT_NE(text.find("Saved Box"), std::string::npos) << text;
    EXPECT_FALSE(fs::exists(Assets() / "scenes" / "Main.scene.tmp"));
}

TEST_F(EditorProjectTest, EveryMutatingCommandMovesTheRevision)
{
    Open("res://scenes/Main.scene");
    uint32_t revision = OpenSceneInfo().revision;
    const auto moved = [&](const char *what)
    {
        const uint32_t now = OpenSceneInfo().revision;
        EXPECT_GT(now, revision) << what;
        revision = now;
    };

    const Frame created = Execute(server, CommandType::CreateEntity, Strings({"Box"}));
    ASSERT_EQ(created.type, static_cast<uint8_t>(ResponseType::EntityCreated));
    moved("CreateEntity");
    BufferReader idReader(created.payload);
    const std::string id = idReader.ReadString();

    // CreateEntity makes an object without a transform (until E4's presets), which SetEntityTransform refuses: give
    // it one directly. That changes nothing the revision tracks.
    const auto object = SceneManager::GetCurSceneRef().FindGameObjectByUUID(Math::UUID::FromString(id).value());
    ASSERT_NE(object, nullptr);
    object->CreatePositionable();
    EXPECT_EQ(OpenSceneInfo().revision, revision);

    BufferWriter transform;
    transform.WriteString(id);
    for (int i = 0; i < 9; ++i)
        transform.WriteF32(1.0f);
    const Frame transformed = Execute(server, CommandType::SetEntityTransform, transform.Release());
    ASSERT_EQ(transformed.type, static_cast<uint8_t>(ResponseType::Ok)) << transformed.Text();
    moved("SetEntityTransform");

    ASSERT_EQ(Execute(server, CommandType::DestroyEntity, Strings({id})).type, static_cast<uint8_t>(ResponseType::Ok));
    moved("DestroyEntity");

    // Reads and failures don't
    (void)Execute(server, CommandType::GetAllEntities);
    (void)Execute(server, CommandType::DestroyEntity, Strings({id}));
    EXPECT_EQ(OpenSceneInfo().revision, revision);
}

TEST_F(EditorProjectTest, SaveAsMakesTheNewFileTheScenesFile)
{
    const SceneInfo opened = Open("res://scenes/Main.scene");
    const SceneInfo saved = DecodeSceneInfo(Save("res://scenes/sub/Copy.scene"));
    EXPECT_EQ(saved.path, "res://scenes/sub/Copy.scene");
    EXPECT_EQ(saved.uuid, IO::ResourceUUID::FromPath(IO::ResourcePath("res://scenes/sub/Copy.scene")).ToString());
    EXPECT_NE(saved.uuid, opened.uuid);
    EXPECT_EQ(saved.name, "Main") << "the scene keeps its name";

    EXPECT_TRUE(fs::is_regular_file(Assets() / "scenes" / "sub" / "Copy.scene")) << "folders are created";
    EXPECT_TRUE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath("res://scenes/sub/Copy.scene")))
        << "the new file is indexed";
    EXPECT_TRUE(fs::exists(_root / ".import" / "scenes" / "sub" / "Copy.scene.meta"));

    // Later saves go to the new file
    EXPECT_EQ(DecodeSceneInfo(Save()).path, "res://scenes/sub/Copy.scene");
}

TEST_F(EditorProjectTest, SaveSceneToFileRefusesBadPaths)
{
    Open("res://scenes/Main.scene");
    for (const std::string path : {"scenes/Main.scene", "res://Main.txt", "res://../x.scene", "user://x.scene"})
    {
        EXPECT_EQ(Save(path).type, ErrorType) << path;
    }
    EXPECT_FALSE(fs::exists(_root / "x.scene"));
}

TEST_F(EditorProjectTest, SavingAndOpeningRoundTripsTheScene)
{
    Open("res://scenes/Main.scene");
    ASSERT_EQ(Execute(server, CommandType::CreateEntity, Strings({"Round Trip"})).type,
              static_cast<uint8_t>(ResponseType::EntityCreated));
    ASSERT_EQ(Save().type, SceneInfoType);

    // Another scene, then back
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", ""})).type, SceneInfoType);
    EXPECT_FALSE(SceneHasObjectNamed("Round Trip"));
    Open("res://scenes/Main.scene");
    EXPECT_TRUE(SceneHasObjectNamed("Round Trip"));
}

// ==================== NewScene ====================

TEST_F(EditorProjectTest, NewSceneWithAPathWritesTheFile)
{
    const SceneInfo info = DecodeSceneInfo(Execute(server, CommandType::NewScene,
                                                   Strings({"res://scenes/Level2.scene", ""})));
    EXPECT_EQ(info.path, "res://scenes/Level2.scene");
    EXPECT_EQ(info.name, "Level2") << "the file's name by default";
    EXPECT_EQ(info.revision, info.savedRevision);
    EXPECT_EQ(info.uuid, IO::ResourceUUID::FromPath(IO::ResourcePath("res://scenes/Level2.scene")).ToString());

    const json written = json::parse(ReadFile(Assets() / "scenes" / "Level2.scene"));
    EXPECT_EQ(written, (json{{"name", "Level2"}, {"rootGameObjects", json::array()}}));
    EXPECT_TRUE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath("res://scenes/Level2.scene")));

    const SceneInfo named = DecodeSceneInfo(Execute(server, CommandType::NewScene,
                                                    Strings({"res://scenes/Other.scene", "Boss Fight"})));
    EXPECT_EQ(named.name, "Boss Fight");
}

TEST_F(EditorProjectTest, NewSceneNeverOverwritesAFile)
{
    const std::string before = ReadFile(Assets() / "scenes" / "Main.scene");
    const Frame response = Execute(server, CommandType::NewScene, Strings({"res://scenes/Main.scene", "Clobber"}));
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.Text().find("already exists"), std::string::npos) << response.Text();
    EXPECT_EQ(ReadFile(Assets() / "scenes" / "Main.scene"), before);
}

TEST_F(EditorProjectTest, AnUntitledSceneHasNoFileUntilSavedWithAPath)
{
    const SceneInfo untitled = DecodeSceneInfo(Execute(server, CommandType::NewScene, Strings({"", ""})));
    EXPECT_TRUE(untitled.path.empty());
    EXPECT_EQ(untitled.name, "Untitled");
    EXPECT_EQ(untitled.revision, untitled.savedRevision);

    const Frame noPath = Save();
    EXPECT_EQ(noPath.type, ErrorType);
    EXPECT_NE(noPath.Text().find("no file"), std::string::npos) << noPath.Text();

    const SceneInfo saved = DecodeSceneInfo(Save("res://scenes/Untitled.scene"));
    EXPECT_EQ(saved.path, "res://scenes/Untitled.scene");
    EXPECT_EQ(saved.uuid, IO::ResourceUUID::FromPath(IO::ResourcePath("res://scenes/Untitled.scene")).ToString());
}

TEST_F(EditorProjectTest, ASceneSentWithLoadSceneHasNoFileAndIsUnsaved)
{
    Open("res://scenes/Main.scene");
    ASSERT_EQ(Execute(server, CommandType::LoadScene, Strings({R"({"name":"Sent","rootGameObjects":[]})"})).type,
              static_cast<uint8_t>(ResponseType::Ok));
    const SceneInfo info = OpenSceneInfo();
    EXPECT_EQ(info.name, "Sent");
    EXPECT_TRUE(info.path.empty());
    EXPECT_NE(info.revision, info.savedRevision);
    EXPECT_EQ(Save().type, ErrorType) << "no file to save to";
}

// ==================== Events ====================

TEST_F(EditorProjectTest, SceneChangesAndSavesPushSceneChangedEvents)
{
    Open("res://scenes/Main.scene");
    (void)Execute(server, CommandType::CreateEntity, Strings({"Box"}));
    (void)Save();

    const std::vector<json> events = EventsOfKind(server, "sceneChanged");
    ASSERT_GE(events.size(), 3u);
    const json &opened = events[events.size() - 3];
    const json &edited = events[events.size() - 2];
    const json &saved = events.back();
    EXPECT_EQ(opened.at("path"), "res://scenes/Main.scene");
    EXPECT_EQ(opened.at("revision"), opened.at("savedRevision"));
    EXPECT_GT(edited.at("revision").get<uint32_t>(), edited.at("savedRevision").get<uint32_t>());
    EXPECT_EQ(saved.at("revision"), edited.at("revision"));
    EXPECT_EQ(saved.at("savedRevision"), saved.at("revision"));
}

TEST_F(EditorProjectTest, RescanAssetsReportsADeletedAssetAndForgetsIt)
{
    WriteAsset("scenes/Doomed.scene", R"({"name":"Doomed","rootGameObjects":[]})");
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, static_cast<uint8_t>(ResponseType::Ok));
    ASSERT_TRUE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath("res://scenes/Doomed.scene")));

    fs::remove(Assets() / "scenes" / "Doomed.scene");
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, static_cast<uint8_t>(ResponseType::Ok));
    EXPECT_FALSE(IO::ResourceLoader::Instance().Exists(IO::ResourcePath("res://scenes/Doomed.scene")));

    const std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].at("added"), json::array({"res://scenes/Doomed.scene"}));
    EXPECT_EQ(events[1].at("removed"), json::array({"res://scenes/Doomed.scene"}));
    EXPECT_EQ(events[1].at("added"), json::array());

    // Nothing changed: no event
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, static_cast<uint8_t>(ResponseType::Ok));
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), 2u);
}

// ==================== Project commands ====================

TEST_F(EditorProjectTest, GetProjectInfoDescribesTheProject)
{
    const ProjectInfo info = DecodeProjectInfo(Execute(server, CommandType::GetProjectInfo));
    EXPECT_EQ(fs::path(std::u8string(info.rootPath.begin(), info.rootPath.end())), _root);
    EXPECT_EQ(info.userDataPath, Utf8(_project.UserDataPath(_base / "user")));
    EXPECT_EQ(info.project, _project.ToJson());
    EXPECT_EQ(info.project.at("name"), "My Game");
}

TEST_F(EditorProjectTest, HelloReportsTheProject)
{
    BufferWriter hello;
    hello.WriteString("test");
    hello.WriteString(std::string(ProtocolVersion));
    hello.WriteString("");
    const Frame response = Execute(server, CommandType::Hello, hello.Release());
    ASSERT_EQ(response.type, static_cast<uint8_t>(ResponseType::ServerInfo));
    EXPECT_EQ(response.payload.back(), uint8_t{1}) << "projectLoaded is the last field";
}

TEST_F(EditorProjectTest, SetProjectSettingsMergesAppliesAndSaves)
{
    // An unknown key at the top and an unknown block in settings survive
    IO::ProjectFile withExtras = _project;
    withExtras.unknownKeys["editorHost"] = "custom.exe";
    withExtras.settings["future"] = {{"keep", true}};
    ASSERT_TRUE(withExtras.Save(_root));
    server.SetProject(_root, withExtras);

    const ProjectInfo info = DecodeProjectInfo(Execute(server, CommandType::SetProjectSettings,
        JsonPayload({{"physics", {{"fixedTimestep", 0.01}}}, {"window", {{"title", "T"}}}})));
    EXPECT_EQ(info.project.at("settings").at("physics").at("fixedTimestep").get<double>(), 0.01);
    EXPECT_EQ(info.project.at("settings").at("window").at("title"), "T");
    EXPECT_EQ(info.project.at("settings").at("future").at("keep"), true);
    EXPECT_EQ(info.project.at("editorHost"), "custom.exe");

    const auto saved = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(saved) << saved.error();
    EXPECT_EQ(saved->ToJson(), info.project) << "what is answered is what was saved";

    // null removes a key (a merge patch)
    const ProjectInfo removed = DecodeProjectInfo(Execute(server, CommandType::SetProjectSettings,
                                                          JsonPayload({{"window", nullptr}})));
    EXPECT_FALSE(removed.project.at("settings").contains("window"));
    EXPECT_TRUE(removed.project.at("settings").contains("physics"));

    EXPECT_FALSE(EventsOfKind(server, "projectChanged").empty());
}

TEST_F(EditorProjectTest, SetProjectSettingsRefusesABlockItsSubsystemRefuses)
{
    const std::string before = ReadFile(IO::ProjectFile::PathIn(_root));

    const Frame refused = Execute(server, CommandType::SetProjectSettings,
                                  JsonPayload({{"physics", {{"fixedTimestep", -1}}}}));
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("physics.fixedTimestep"), std::string::npos) << refused.Text();

    const Frame layers = Execute(server, CommandType::SetProjectSettings, JsonPayload({{"layers", "nope"}}));
    EXPECT_EQ(layers.type, ErrorType);
    EXPECT_NE(layers.Text().find("layers"), std::string::npos) << layers.Text();

    EXPECT_EQ(Execute(server, CommandType::SetProjectSettings, JsonPayload(json::array({1}))).type, ErrorType)
        << "a patch must be an object";
    EXPECT_EQ(Execute(server, CommandType::SetProjectSettings, Strings({"{ not json"})).type, ErrorType);

    EXPECT_EQ(ReadFile(IO::ProjectFile::PathIn(_root)), before) << "nothing refused is saved";
}

TEST_F(EditorProjectTest, ARefusedPatchPutsTheLiveSettingsBack)
{
    // The project has no physics or layers block: rolling back to "the previous settings" would apply nothing, so the
    // live values themselves must come back
    ASSERT_FALSE(_project.settings.contains("physics"));
    const double timestep = Time::GetFixedTimestep();
    const std::string layerName = Layers::LayerToName(8);

    json layers = Layers::Serialize();
    layers["layers"][8] = "Patched Layer";
    const Frame refused = Execute(server, CommandType::SetProjectSettings,
                                  JsonPayload({{"physics", {{"fixedTimestep", timestep * 2}}},
                                               {"layers", layers},
                                               {"input", "not an input block"}}));
    ASSERT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("input"), std::string::npos) << refused.Text();

    EXPECT_EQ(Time::GetFixedTimestep(), timestep) << "the timestep the patch applied is undone";
    EXPECT_EQ(Layers::LayerToName(8), layerName) << "and the layer names";

    // An accepted patch does change them
    const ProjectInfo accepted = DecodeProjectInfo(Execute(server, CommandType::SetProjectSettings,
        JsonPayload({{"physics", {{"fixedTimestep", timestep * 2}}}, {"layers", layers}})));
    EXPECT_EQ(accepted.project.at("settings").at("physics").at("fixedTimestep").get<double>(), timestep * 2);
    EXPECT_EQ(Time::GetFixedTimestep(), timestep * 2);
    EXPECT_EQ(Layers::LayerToName(8), "Patched Layer");

    // Put the process-wide state back for the other tests
    Layers::ResetToDefaults();
    ASSERT_TRUE(Time::SetFixedTimestep(timestep));
}

TEST_F(EditorProjectTest, SetStartupSceneSavesItAndAddsItToTheSceneList)
{
    WriteAsset("scenes/Level2.scene", R"({"name":"Level2","rootGameObjects":[]})");

    const ProjectInfo info = DecodeProjectInfo(Execute(server, CommandType::SetStartupScene,
                                                       Strings({"res://scenes/./Level2.scene"})));
    EXPECT_EQ(info.project.at("startupScene"), "res://scenes/Level2.scene") << "normalised";
    EXPECT_EQ(info.project.at("scenes"), json::array({"res://scenes/Main.scene", "res://scenes/Level2.scene"}));
    const auto saved = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(saved) << saved.error();
    EXPECT_EQ(saved->startupScene, "res://scenes/Level2.scene");

    // Already listed: not twice
    const ProjectInfo again = DecodeProjectInfo(Execute(server, CommandType::SetStartupScene,
                                                        Strings({"res://scenes/Main.scene"})));
    EXPECT_EQ(again.project.at("scenes").size(), 2u);

    const ProjectInfo cleared = DecodeProjectInfo(Execute(server, CommandType::SetStartupScene, Strings({""})));
    EXPECT_EQ(cleared.project.at("startupScene"), "");
}

TEST_F(EditorProjectTest, SetStartupSceneRefusesWhatIsntAnExistingSceneFile)
{
    for (const std::string path : {"res://scenes/Missing.scene", "res://scenes/Main.json", "scenes/Main.scene",
                                   "res://../Main.scene"})
    {
        EXPECT_EQ(Execute(server, CommandType::SetStartupScene, Strings({path})).type, ErrorType) << path;
    }
    const auto saved = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->startupScene, IO::DefaultStartupScene);
}

// ==================== Without a project ====================

TEST(EditorNoProjectTest, ProjectAndSceneFileCommandsNeedAProject)
{
    EditorServer server;
    EXPECT_FALSE(server.HasProject());
    for (const auto &[command, payload] :
         std::vector<std::pair<CommandType, std::vector<uint8_t>>>{
             {CommandType::GetProjectInfo, {}},
             {CommandType::SetProjectSettings, JsonPayload(json::object())},
             {CommandType::SetStartupScene, Strings({"res://scenes/Main.scene"})},
             {CommandType::OpenScene, Strings({"res://scenes/Main.scene"})},
             {CommandType::NewScene, Strings({"res://scenes/New.scene", ""})},
         })
    {
        const Frame response = Execute(server, command, payload);
        EXPECT_EQ(response.type, ErrorType) << static_cast<int>(command);
        EXPECT_NE(response.Text().find("No project"), std::string::npos) << response.Text();
    }
}

// ==================== ResolveScenePath ====================

TEST(EditorScenePathTest, OnlyScenePathsInsideTheAssetsFolderResolve)
{
    const fs::path assets = fs::temp_directory_path() / "n2-scene-path-test" / "assets";
    const auto resolved = EditorServer::ResolveScenePath(assets, "res://levels/../scenes/Main.scene");
    ASSERT_TRUE(resolved) << resolved.error();
    EXPECT_EQ(resolved->resourcePath.ToString(), "res://scenes/Main.scene");
    EXPECT_EQ(resolved->file.filename(), "Main.scene");
    EXPECT_EQ(resolved->file.parent_path().filename(), "scenes");

    for (const std::string path : {"", "res://", "Main.scene", "res://../Main.scene", "res://a/../../Main.scene",
                                   "res://Main.txt", "user://Main.scene"})
    {
        EXPECT_FALSE(EditorServer::ResolveScenePath(assets, path)) << path;
    }
}

// ==================== --create ====================

TEST(EditorCreateTest, RunCreateMakesAProjectAndRefusesToMakeItTwice)
{
    const fs::path base = fs::temp_directory_path() / "n2-run-create-test";
    std::error_code error;
    fs::remove_all(base, error);

    HostOptions options;
    options.createPath = Utf8(base / "Made By Create");
    options.projectName = "Created";
    EXPECT_EQ(RunCreate(options), 0);
    const auto project = IO::ProjectFile::Load(base / "Made By Create");
    ASSERT_TRUE(project) << project.error();
    EXPECT_EQ(project->name, "Created");
    EXPECT_EQ(project->startupScene, IO::DefaultStartupScene);

    EXPECT_EQ(RunCreate(options), ExitCodeAlreadyAProject);

    // from-path: the namespace the folder's assets had before projects had ids
    HostOptions adopt;
    adopt.createPath = Utf8(base / "Adopted");
    adopt.projectIdFromPath = true;
    EXPECT_EQ(RunCreate(adopt), 0);
    const auto adopted = IO::ProjectFile::Load(base / "Adopted");
    ASSERT_TRUE(adopted) << adopted.error();
    EXPECT_EQ(adopted->projectId, IO::ResourceUUID::NamespaceForProjectDir(base / "Adopted"));

    // A file where the folder should be: any other failure is 1
    std::ofstream(base / "file") << "x";
    HostOptions onAFile;
    onAFile.createPath = Utf8(base / "file");
    EXPECT_EQ(RunCreate(onAFile), 1);

    fs::remove_all(base, error);
}
