#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/io/AssetMetadata.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/scripting/LuaComponent.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/LuaScript.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
using namespace N2Engine::Scripting;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// MoveAsset (editor protocol 1.24.0) through EditorServer::ExecuteCommand, against a project made by IO::CreateProject:
// moved assets keep their UUIDs (so a scene that references one by UUID still resolves it), the answer, the
// assetsChanged event and the watcher, project.n2proj, the open scene's scripts, the warnings, every refusal (nothing
// changed), the all-or-nothing rollback, case-only renames, and the read-only host. Part of editor_project_tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t AssetsMovedType = static_cast<uint8_t>(ResponseType::AssetsMoved);
    constexpr uint8_t AssetDetailType = static_cast<uint8_t>(ResponseType::AssetDetail);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);

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

    /// The names in a folder as the file system spells them
    std::set<std::string> NamesIn(const fs::path &folder)
    {
        std::set<std::string> names;
        std::error_code error;
        for (const auto &entry : fs::directory_iterator(folder, error))
            names.insert(IO::PathToUtf8(entry.path().filename()));
        return names;
    }

    class EditorAssetMoveTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-move-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().SetReadOnly(false);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            server.SetAssetPollInterval(std::chrono::milliseconds(0));
        }

        void TearDown() override
        {
            server.SetMoveRenameForTest(nullptr);
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }
        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }
        [[nodiscard]] fs::path Meta(const std::string &relative) const { return _root / ".import" / (relative + ".meta"); }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = Assets() / IO::PathFromUtf8(relative);
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        void Rescan() { ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType); }

        Frame Move(const json &moves)
        {
            BufferWriter w;
            WriteJson(w, moves);
            return Execute(server, CommandType::MoveAsset, w.Release());
        }

        Frame MoveOne(const std::string &from, const std::string &to)
        {
            return Move(json::array({{{"from", from}, {"to", to}}}));
        }

        static json Result(const Frame &frame)
        {
            EXPECT_EQ(frame.type, AssetsMovedType) << frame.Text();
            if (frame.type != AssetsMovedType)
                return json::object();
            BufferReader r(frame.payload);
            json result = ReadJson(r);
            EXPECT_FALSE(r.HasData());
            return result;
        }

        json Info(const std::string &uuidOrPath)
        {
            const Frame frame = Execute(server, CommandType::GetAssetInfo, Strings({uuidOrPath}));
            EXPECT_EQ(frame.type, AssetDetailType) << frame.Text();
            if (frame.type != AssetDetailType)
                return json::object();
            BufferReader r(frame.payload);
            return ReadJson(r);
        }

        std::string OpenSceneUuid(const std::string &path)
        {
            const Frame frame = Execute(server, CommandType::OpenScene, Strings({path}));
            EXPECT_EQ(frame.type, SceneInfoType) << frame.Text();
            if (frame.type != SceneInfoType)
                return {};
            BufferReader r(frame.payload);
            (void)r.ReadString();
            (void)r.ReadString();
            return r.ReadString();
        }

        /// A new scene with no file, so no scene file is open
        void CloseSceneFile() { ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Scratch"})).type, SceneInfoType); }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

// ==================== What a move keeps ====================

TEST_F(EditorAssetMoveTest, AMovedFileKeepsItsUuidAndTheAnswerAndTheEventCarryThePair)
{
    WriteAsset("a.mat", "{}");
    Rescan();
    const std::string uuid = Info("res://a.mat").at("uuid").get<std::string>();
    const std::size_t eventsBefore = EventsOfKind(server, "assetsChanged").size();

    const json result = Result(MoveOne("res://a.mat", "res://materials/b.mat"));
    ASSERT_EQ(result.at("moved").size(), 1u);
    EXPECT_EQ(result.at("moved")[0],
              (json{{"from", "res://a.mat"}, {"to", "res://materials/b.mat"}, {"uuid", uuid}, {"type", "Material"}}));
    EXPECT_TRUE(result.at("warnings").empty());

    EXPECT_TRUE(fs::exists(Assets() / "materials" / "b.mat")) << "the missing folder was made";
    EXPECT_FALSE(fs::exists(Assets() / "a.mat"));
    EXPECT_FALSE(fs::exists(Meta("a.mat")));
    EXPECT_EQ(json::parse(ReadFile(Meta("materials/b.mat"))).at("uuid"), uuid);
    EXPECT_EQ(Info(uuid).at("path"), "res://materials/b.mat") << "the same UUID names the new path";

    const std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), eventsBefore + 1);
    EXPECT_EQ(events.back().at("removed"), json::array({"res://a.mat"})) << "an old client sees a file leave";
    EXPECT_EQ(events.back().at("added"), json::array({"res://materials/b.mat"})) << "and one arrive";
    EXPECT_EQ(events.back().at("moved"), result.at("moved"));

    EXPECT_FALSE(server.PollAssets()) << "the watcher finds nothing the command did not already report";
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), eventsBefore + 1);
}

TEST_F(EditorAssetMoveTest, AFolderMovesWithEverythingInItAndIsListedFirst)
{
    WriteAsset("art/a.mat", "{}");
    WriteAsset("art/sub/b.mat", "{}");
    WriteAsset("art/readme.txt", "not an asset");
    Rescan();
    const std::string a = Info("res://art/a.mat").at("uuid").get<std::string>();

    const json result = Result(MoveOne("res://art", "res://library/art"));
    const json &moved = result.at("moved");
    ASSERT_EQ(moved.size(), 3u);
    EXPECT_EQ(moved[0], (json{{"from", "res://art"}, {"to", "res://library/art"}})) << "the folder, no UUID";
    EXPECT_EQ(moved[1].at("to"), "res://library/art/a.mat");
    EXPECT_EQ(moved[1].at("uuid"), a);
    EXPECT_EQ(moved[2].at("to"), "res://library/art/sub/b.mat");
    EXPECT_TRUE(fs::exists(Assets() / "library" / "art" / "readme.txt"));
    EXPECT_EQ(Info(a).at("path"), "res://library/art/a.mat");
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").back().at("moved").size(), 2u) << "assets only";
}

TEST_F(EditorAssetMoveTest, ASceneThatReferencesAMovedScriptByUuidStillResolvesIt)
{
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    WriteAsset("scripts/Hot.lua", "return {}\n");
    Rescan();
    const IO::ResourcePath script("res://scripts/Hot.lua");
    const Math::UUID scriptUuid = Loader().GetUUID(script);

    // A scene file whose object uses the script, saved with its scriptUUID, then closed
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"res://scenes/Level.scene", "Level"})).type, SceneInfoType);
    {
        const auto object = GameObject::Create("Runner");
        SceneManager::GetCurSceneRef().AddRootGameObject(object);
        object->AddComponent<LuaComponent>()->SetScript(script);
    }
    ASSERT_EQ(Execute(server, CommandType::SaveSceneToFile, Strings({""})).type, SceneInfoType);
    ASSERT_NE(ReadFile(Assets() / "scenes" / "Level.scene").find(scriptUuid.ToString()), std::string::npos);
    CloseSceneFile();

    ASSERT_EQ(Result(MoveOne("res://scripts/Hot.lua", "res://lib/Hot.lua")).at("moved").size(), 1u);

    (void)OpenSceneUuid("res://scenes/Level.scene");
    LuaComponent *found = nullptr;
    for (const auto &object : SceneManager::GetCurSceneRef().GetAllGameObjects())
    {
        if (auto *lua = object->GetComponent<LuaComponent>())
            found = lua;
    }
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->GetScriptPath(), IO::ResourcePath("res://lib/Hot.lua")) << "resolved by UUID to the new path";
    EXPECT_FALSE(found->HasMissingScript());

    // Saved again, the scene names the same UUID
    ASSERT_EQ(Execute(server, CommandType::SaveSceneToFile, Strings({""})).type, SceneInfoType);
    EXPECT_NE(ReadFile(Assets() / "scenes" / "Level.scene").find(scriptUuid.ToString()), std::string::npos);
}

TEST_F(EditorAssetMoveTest, AMovedSceneKeepsItsUuidAndTheProjectFileFollowsIt)
{
    const std::string uuid = OpenSceneUuid("res://scenes/Main.scene");
    ASSERT_FALSE(uuid.empty());
    CloseSceneFile();

    ASSERT_EQ(Result(MoveOne("res://scenes", "res://levels")).at("moved").size(), 2u);

    const auto project = IO::ProjectFile::Load(_root);
    ASSERT_TRUE(project) << project.error();
    EXPECT_EQ(project->startupScene, "res://levels/Main.scene");
    EXPECT_EQ(project->scenes, (std::vector<std::string>{"res://levels/Main.scene"}));
    EXPECT_FALSE(EventsOfKind(server, "projectChanged").empty());
    EXPECT_EQ(OpenSceneUuid("res://levels/Main.scene"), uuid) << "a scene's UUID is its file's, kept by the move";
}

TEST_F(EditorAssetMoveTest, ARunningScriptFollowsItsFileAndKeepsItsState)
{
    sol::state &lua = LuaRuntime::Instance().GetState();
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    lua["move_runs"] = 0;
    WriteAsset("scripts/Runner.lua", "move_runs = move_runs + 1\nreturn {}\n");
    Rescan();
    const IO::ResourcePath from("res://scripts/Runner.lua");
    const IO::ResourcePath to("res://ai/Runner.lua");
    CloseSceneFile();
    const auto object = GameObject::Create("Runner");
    SceneManager::GetCurSceneRef().AddRootGameObject(object);
    auto *component = object->AddComponent<LuaComponent>();
    component->SetScriptData(json{{"kept", 7}});
    component->SetScript(from);
    const int runsBefore = lua["move_runs"].get<int>();
    const auto held = Loader().GetCached<LuaScript>(from);
    ASSERT_NE(held, nullptr);

    ASSERT_EQ(Result(MoveOne(from.ToString(), to.ToString())).at("moved").size(), 1u);

    EXPECT_EQ(component->GetScriptPath(), to);
    EXPECT_EQ(lua["move_runs"].get<int>(), runsBefore) << "nothing ran again";
    EXPECT_EQ(component->GetScriptData().value("kept", 0), 7);
    EXPECT_EQ(Loader().GetCached<LuaScript>(to), held) << "the same script object";

    // A write of the new file reloads the component: its reload subscription followed the file
    ASSERT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({to.ToString(), "move_runs = move_runs + 10\nreturn {}\n"})).type,
              OkType);
    EXPECT_GT(lua["move_runs"].get<int>(), runsBefore + 10);
}

TEST_F(EditorAssetMoveTest, ScriptsThatStillNameTheOldPathsAreWarnedAbout)
{
    WriteAsset("scripts/util.lua", "return {}\n");
    WriteAsset("scripts/user.lua", "local util = require(\"scripts.util\")\nreturn {}\n");
    WriteAsset("other.lua", "local tex = 'res://scripts/util.lua'\nreturn {}\n");
    Rescan();

    const json result = Result(MoveOne("res://scripts/util.lua", "res://lib/util.lua"));
    const json &warnings = result.at("warnings");
    const auto has = [&](const std::string &text)
    {
        for (const json &warning : warnings)
            if (warning.get<std::string>() == text)
                return true;
        return false;
    };
    EXPECT_TRUE(has("res://scripts/user.lua still names scripts.util")) << warnings.dump();
    EXPECT_TRUE(has("res://other.lua still names res://scripts/util.lua")) << warnings.dump();
    EXPECT_TRUE(fs::exists(Assets() / "lib" / "util.lua")) << "a warning does not stop the move";
}

TEST_F(EditorAssetMoveTest, ACaseOnlyRenameKeepsTheUuidAndTakesTheNewSpelling)
{
    WriteAsset("tex/Stone.mat", "{}");
    Rescan();
    const std::string uuid = Info("res://tex/Stone.mat").at("uuid").get<std::string>();

    const json result = Result(MoveOne("res://tex/Stone.mat", "res://tex/stone.mat"));
    ASSERT_EQ(result.at("moved").size(), 1u);
    EXPECT_EQ(result.at("moved")[0].at("to"), "res://tex/stone.mat");
    EXPECT_EQ(NamesIn(Assets() / "tex"), (std::set<std::string>{"stone.mat"}));
    EXPECT_EQ(NamesIn(_root / ".import" / "tex"), (std::set<std::string>{"stone.mat.meta"}));
    EXPECT_EQ(Info(uuid).at("path"), "res://tex/stone.mat");
}

// ==================== Refusals: nothing changes ====================

TEST_F(EditorAssetMoveTest, EveryRefusalLeavesTheDiskAndTheIndexAsTheyWere)
{
    WriteAsset("a.mat", "{}");
    WriteAsset("b.mat", "{}");
    WriteAsset("dir/c.mat", "{}");
    WriteAsset("dir/sub/d.mat", "{}");
    WriteAsset("notes.txt", "text");
    WriteAsset("Folder/x.mat", "{}");
    Rescan();
    (void)OpenSceneUuid("res://scenes/Main.scene");
    const std::string aUuid = Info("res://a.mat").at("uuid").get<std::string>();
    const std::string metaBefore = ReadFile(Meta("a.mat"));

    const auto pair = [](const std::string &from, const std::string &to) { return json{{"from", from}, {"to", to}}; };
    const std::vector<std::pair<json, std::string>> refused = {
        {json::array(), "at least one"},
        {json::object(), "JSON array"},
        {json::array({json{{"from", "res://a.mat"}}}), "from and to"},
        {json::array({pair("res://a.mat", "res://b.mat")}), "already exists"},
        {json::array({pair("res://a.mat", "res://B.MAT")}), ""},
        {json::array({pair("res://a.mat", "res://folder")}), ""},
        {json::array({pair("res://a.mat", "res://a.mat")}), "already there"},
        {json::array({pair("res://a.mat", "res://a.png")}), "extension"},
        {json::array({pair("res://dir", "res://dir/sub/dir")}), "into itself"},
        {json::array({pair("res://dir", "res://DIR/inner")}), "into itself"},
        {json::array({pair("res://dir", "res://x"), pair("res://dir/c.mat", "res://c.mat")}), "overlap"},
        {json::array({pair("res://a.mat", "res://n/a.mat"), pair("res://b.mat", "res://n/a.mat")}), "overlap"},
        {json::array({pair("res://scenes/Main.scene", "res://Main.scene")}), "open scene"},
        {json::array({pair("res://scenes", "res://levels")}), "open scene"},
        {json::array({pair("res://missing.mat", "res://m.mat")}), "Not found"},
        {json::array({pair("res://notes.txt", "res://n.txt")}), "Not an asset"},
        {json::array({pair("res://../a.mat", "res://n.mat")}), ""},
        {json::array({pair("res://a.mat", "res://bad:name.mat")}), ""},
        {json::array({pair("res://", "res://x")}), ""},
        {json::array({pair("C:/a.mat", "res://x.mat")}), ""},
    };
    for (const auto &[moves, expected] : refused)
    {
        const Frame response = Move(moves);
        EXPECT_EQ(response.type, ErrorType) << moves.dump();
        if (!expected.empty())
            EXPECT_NE(response.Text().find(expected), std::string::npos) << moves.dump() << ": " << response.Text();
    }
    json tooMany = json::array();
    for (int i = 0; i < 257; ++i)
        tooMany.push_back(pair("res://a.mat", "res://n" + std::to_string(i) + ".mat"));
    EXPECT_EQ(Move(tooMany).type, ErrorType);

    EXPECT_TRUE(fs::exists(Assets() / "a.mat"));
    EXPECT_TRUE(fs::exists(Assets() / "dir" / "sub" / "d.mat"));
    EXPECT_FALSE(fs::exists(Assets() / "n"));
    EXPECT_EQ(ReadFile(Meta("a.mat")), metaBefore);
    EXPECT_EQ(Info(aUuid).at("path"), "res://a.mat");
}

TEST_F(EditorAssetMoveTest, AFailureMovesTheEarlierEntriesBackAndNamesTheOneThatFailed)
{
    WriteAsset("one.mat", "{}");
    WriteAsset("two.mat", "{}");
    WriteAsset("three.mat", "{}");
    Rescan();
    const std::string one = Info("res://one.mat").at("uuid").get<std::string>();
    server.SetMoveRenameForTest([](const fs::path &from, const fs::path &to) -> std::expected<void, std::string>
    {
        if (from.filename() == "two.mat")
            return std::unexpected("the file is open in another program");
        std::error_code error;
        fs::rename(from, to, error);
        if (error)
            return std::unexpected(error.message());
        return {};
    });
    const std::size_t eventsBefore = EventsOfKind(server, "assetsChanged").size();

    const Frame response = Move(json::array({{{"from", "res://one.mat"}, {"to", "res://moved/one.mat"}},
                                             {{"from", "res://two.mat"}, {"to", "res://moved/two.mat"}},
                                             {{"from", "res://three.mat"}, {"to", "res://moved/three.mat"}}}));
    ASSERT_EQ(response.type, ErrorType);
    EXPECT_NE(response.Text().find("res://two.mat"), std::string::npos) << response.Text();
    EXPECT_NE(response.Text().find("the file is open in another program"), std::string::npos) << response.Text();
    EXPECT_NE(response.Text().find("nothing was moved"), std::string::npos) << response.Text();

    for (const char *name : {"one.mat", "two.mat", "three.mat"})
    {
        EXPECT_TRUE(fs::exists(Assets() / name)) << name;
        EXPECT_TRUE(fs::exists(Meta(name))) << name;
    }
    EXPECT_FALSE(fs::exists(Assets() / "moved")) << "the folder the move made is gone again";
    EXPECT_EQ(Info(one).at("path"), "res://one.mat");
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), eventsBefore) << "no event for a move that was undone";
}

TEST_F(EditorAssetMoveTest, AReadOnlyHostAndAHostWithoutAProjectRefuseIt)
{
    WriteAsset("a.mat", "{}");
    Rescan();
    ASSERT_TRUE(server.SetReadOnly(true));
    EXPECT_EQ(MoveOne("res://a.mat", "res://b.mat").type, ErrorType);
    ASSERT_TRUE(server.SetReadOnly(false));
    EXPECT_TRUE(fs::exists(Assets() / "a.mat"));

    EditorServer noProject;
    BufferWriter w;
    WriteJson(w, json::array({{{"from", "res://a.mat"}, {"to", "res://b.mat"}}}));
    EXPECT_EQ(Execute(noProject, CommandType::MoveAsset, w.Release()).type, ErrorType);
    EXPECT_EQ(EditorServer::ClassifyCommand(static_cast<uint8_t>(CommandType::MoveAsset)), EditorServer::CommandClass::Write);
    EXPECT_TRUE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::MoveAsset)));
}
