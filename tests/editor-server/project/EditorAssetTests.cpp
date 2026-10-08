#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Logger.hpp>
#include <engine/base/Asset.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourcePath.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/scripting/LuaComponent.hpp>
#include <engine/scripting/LuaRuntime.hpp>
#include <engine/scripting/LuaScript.hpp>
#include <engine/scripting/LuaScriptTemplate.hpp>
#include <math/UUID.hpp>

using namespace N2Engine;
using namespace N2Engine::Scripting;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// The asset commands and the file watcher (#81, E8) through EditorServer::ExecuteCommand, against a project made by
// IO::CreateProject. Part of the editor_project_tests program, which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t AssetListType = static_cast<uint8_t>(ResponseType::AssetList);
    constexpr uint8_t AssetDetailType = static_cast<uint8_t>(ResponseType::AssetDetail);
    constexpr uint8_t TextDataType = static_cast<uint8_t>(ResponseType::TextData);
    constexpr uint8_t AssetCreatedType = static_cast<uint8_t>(ResponseType::AssetCreated);

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

    struct AssetListResult
    {
        std::vector<std::string> folders;
        json assets = json::array();

        [[nodiscard]] std::vector<std::string> Paths() const
        {
            std::vector<std::string> paths;
            for (const json &asset : assets)
                paths.push_back(asset.at("path").get<std::string>());
            return paths;
        }
    };

    AssetListResult DecodeAssetList(const Frame &frame)
    {
        AssetListResult result;
        EXPECT_EQ(frame.type, AssetListType) << frame.Text();
        if (frame.type != AssetListType)
            return result;
        BufferReader r(frame.payload);
        const json folders = ReadJson(r);
        result.assets = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after AssetList";
        for (const json &folder : folders)
            result.folders.push_back(folder.get<std::string>());
        return result;
    }

    json DecodeAssetDetail(const Frame &frame)
    {
        EXPECT_EQ(frame.type, AssetDetailType) << frame.Text();
        if (frame.type != AssetDetailType)
            return json::object();
        BufferReader r(frame.payload);
        json info = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after AssetDetail";
        return info;
    }

    std::string DecodeTextData(const Frame &frame)
    {
        EXPECT_EQ(frame.type, TextDataType) << frame.Text();
        if (frame.type != TextDataType)
            return {};
        BufferReader r(frame.payload);
        std::string text = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after TextData";
        return text;
    }

    struct Created
    {
        std::string path;
        std::string uuid;
    };

    Created DecodeAssetCreated(const Frame &frame)
    {
        Created created;
        EXPECT_EQ(frame.type, AssetCreatedType) << frame.Text();
        if (frame.type != AssetCreatedType)
            return created;
        BufferReader r(frame.payload);
        created.path = r.ReadString();
        created.uuid = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after AssetCreated";
        return created;
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

    std::vector<std::string> StringList(const json &list)
    {
        std::vector<std::string> strings;
        for (const json &item : list)
            strings.push_back(item.get<std::string>());
        return strings;
    }

    /// An asset the loader holds in memory without reading a file, to see what a change does to a cached asset
    class HeldAsset final : public Base::Asset
    {
    public:
        [[nodiscard]] std::string GetResourceType() const override { return "Material"; }
    };

    class EditorAssetsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-assets-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "My Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "My Game");

            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            // The tests call PollAssets and PollAssetsIfDue themselves
            server.SetAssetPollInterval(std::chrono::milliseconds(0));
        }

        void TearDown() override
        {
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        static IO::ResourceLoader &Loader() { return IO::ResourceLoader::Instance(); }
        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }

        void WriteAsset(const std::string &relative, const std::string &text)
        {
            const fs::path path = Assets() / IO::PathFromUtf8(relative);
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << text;
        }

        /// Moves a file's modification time on, as an edit made a little later would
        void Touch(const std::string &relative, const std::chrono::milliseconds by = std::chrono::milliseconds(2000))
        {
            const fs::path path = Assets() / IO::PathFromUtf8(relative);
            fs::last_write_time(path, fs::last_write_time(path) + by);
        }

        AssetListResult List(const std::string &folder, const bool recursive)
        {
            BufferWriter w;
            w.WriteString(folder);
            w.WriteBool(recursive);
            return DecodeAssetList(Execute(server, CommandType::ListAssets, w.Release()));
        }

        Frame Info(const std::string &uuidOrPath) { return Execute(server, CommandType::GetAssetInfo, Strings({uuidOrPath})); }
        Frame Read(const std::string &path) { return Execute(server, CommandType::ReadTextAsset, Strings({path})); }
        Frame Write(const std::string &path, const std::string &text)
        {
            return Execute(server, CommandType::WriteTextAsset, Strings({path, text}));
        }
        Frame Folder(const std::string &path) { return Execute(server, CommandType::CreateFolder, Strings({path})); }
        Frame Script(const std::string &path, const std::string &className)
        {
            return Execute(server, CommandType::CreateScriptAsset, Strings({path, className}));
        }
        Frame SetSettings(const std::string &path, const json &customData)
        {
            BufferWriter w;
            w.WriteString(path);
            WriteJson(w, customData);
            return Execute(server, CommandType::SetImportSettings, w.Release());
        }

        void Rescan() { EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType); }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

// ==================== ListAssets ====================

TEST_F(EditorAssetsTest, ListAssetsListsAFolderWithItsSubfoldersAndFiles)
{
    WriteAsset("root.mat", "{}");
    WriteAsset("deep/er/leaf.mat", "{}");
    fs::create_directories(Assets() / "empty");
    Rescan();

    const AssetListResult top = List("res://", false);
    EXPECT_EQ(top.folders, (std::vector<std::string>{"res://deep", "res://empty", "res://scenes", "res://scripts"}))
        << "folders come from the file system, so an empty one is listed";
    EXPECT_EQ(top.Paths(), (std::vector<std::string>{"res://root.mat"}));
    EXPECT_EQ(List("", false).Paths(), top.Paths()) << "an empty folder is the assets folder";

    const AssetListResult all = List("res://", true);
    EXPECT_EQ(all.folders, (std::vector<std::string>{"res://deep", "res://deep/er", "res://empty", "res://scenes",
                                                     "res://scripts"}));
    EXPECT_EQ(all.Paths(), (std::vector<std::string>{"res://deep/er/leaf.mat", "res://root.mat", "res://scenes/Main.scene",
                                                     "res://scripts/Example.lua"}))
        << "sorted by path";

    const json &root = all.assets.at(1);
    ASSERT_EQ(root.at("path"), "res://root.mat");
    EXPECT_EQ(root.at("uuid"), IO::ResourceUUID::FromPath(IO::ResourcePath("res://root.mat")).ToString());
    EXPECT_EQ(root.at("type"), "Material");
    EXPECT_EQ(root.at("size"), 2u);
    EXPECT_GT(root.at("modified").get<std::uint64_t>(), 0u);
    EXPECT_FALSE(root.contains("subAssets")) << "only a file that has sub-assets lists them";
    EXPECT_EQ(all.assets.at(2).at("type"), "Scene");
    EXPECT_EQ(all.assets.at(3).at("type"), "LuaScript");
}

TEST_F(EditorAssetsTest, ListAssetsInASubfolder)
{
    WriteAsset("deep/one.mat", "{}");
    WriteAsset("deep/er/two.mat", "{}");
    WriteAsset("deeper.mat", "{}"); // a name that starts like the folder's, but is not in it
    Rescan();

    const AssetListResult own = List("res://deep", false);
    EXPECT_EQ(own.folders, (std::vector<std::string>{"res://deep/er"}));
    EXPECT_EQ(own.Paths(), (std::vector<std::string>{"res://deep/one.mat"}));

    const AssetListResult below = List("res://deep/", true);
    EXPECT_EQ(below.folders, (std::vector<std::string>{"res://deep/er"}));
    EXPECT_EQ(below.Paths(), (std::vector<std::string>{"res://deep/er/two.mat", "res://deep/one.mat"}));

    const AssetListResult leaf = List("res://deep/er", true);
    EXPECT_TRUE(leaf.folders.empty());
    EXPECT_EQ(leaf.Paths(), (std::vector<std::string>{"res://deep/er/two.mat"}));
}

TEST_F(EditorAssetsTest, ListAssetsRefusesWhatIsNotAFolderOfTheProject)
{
    for (const std::string &folder : {std::string("res://scenes/Main.scene"), std::string("res://nothing"),
                                      std::string("res://.."), std::string("res://../outside"),
                                      std::string("res://scenes/../../x"), std::string("C:/Windows"),
                                      std::string("scenes"), std::string("user://")})
    {
        BufferWriter request;
        request.WriteString(folder);
        request.WriteBool(false);
        EXPECT_EQ(Execute(server, CommandType::ListAssets, request.Release()).type, ErrorType) << folder;
    }

    EditorServer noProject;
    BufferWriter request;
    request.WriteString("res://");
    request.WriteBool(true);
    const Frame refused = Execute(noProject, CommandType::ListAssets, request.Release());
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_NE(refused.Text().find("project"), std::string::npos) << refused.Text();
}

TEST_F(EditorAssetsTest, ListAssetsShowsTheSubAssetsOfAFileThatHasThem)
{
    WriteAsset("models/robot.mat", "{}"); // any indexed file stands in for a model here
    Rescan();
    const IO::ResourcePath robot("res://models/robot.mat");
    ASSERT_TRUE(Loader().SetSubAssetIndex(
        robot, {{"mesh/Arm", "Mesh", IO::ResourceUUID::FromSubAsset(robot, "mesh/Arm")},
                {"material/Red", "Material", IO::ResourceUUID::FromSubAsset(robot, "material/Red")}}));

    const AssetListResult list = List("res://models", false);
    ASSERT_EQ(list.assets.size(), 1u);
    const json &subAssets = list.assets.at(0).at("subAssets");
    ASSERT_EQ(subAssets.size(), 2u);
    EXPECT_EQ(subAssets.at(0).at("key"), "material/Red");
    EXPECT_EQ(subAssets.at(0).at("type"), "Material");
    EXPECT_EQ(subAssets.at(0).at("uuid"), IO::ResourceUUID::FromSubAsset(robot, "material/Red").ToString());
    EXPECT_EQ(subAssets.at(1).at("key"), "mesh/Arm");
}

// ==================== GetAssetInfo ====================

TEST_F(EditorAssetsTest, GetAssetInfoByPathAndByUuid)
{
    const std::string path = "res://scripts/Example.lua";
    const json byPath = DecodeAssetDetail(Info(path));
    EXPECT_EQ(byPath.at("path"), path);
    EXPECT_EQ(byPath.at("type"), "LuaScript");
    EXPECT_EQ(byPath.at("customData"), json::object()) << "no import settings";
    EXPECT_EQ(byPath.at("loaded"), false);
    EXPECT_FALSE(byPath.contains("subAssets"));

    const json byUuid = DecodeAssetDetail(Info(byPath.at("uuid").get<std::string>()));
    EXPECT_EQ(byUuid, byPath);

    const auto script = Loader().Load<LuaScript>(IO::ResourcePath(path));
    ASSERT_NE(script, nullptr);
    EXPECT_EQ(DecodeAssetDetail(Info(path)).at("loaded"), true);
}

TEST_F(EditorAssetsTest, GetAssetInfoHasTheImportSettingsWithoutTheSubAssetIndex)
{
    WriteAsset("a.mat", "{}");
    Rescan();
    const IO::ResourcePath path("res://a.mat");
    ASSERT_TRUE(Loader().SetSubAssetIndex(path, {{"mesh/Arm", "Mesh", IO::ResourceUUID::FromSubAsset(path, "mesh/Arm")}}));
    ASSERT_TRUE(Loader().SetImportSettings(path, json{{"material", {{"note", "x"}}}}));

    const json info = DecodeAssetDetail(Info("res://a.mat"));
    EXPECT_EQ(info.at("customData"), (json{{"material", {{"note", "x"}}}})) << "what SetImportSettings takes back";
    EXPECT_EQ(info.at("subAssets").size(), 1u);
}

TEST_F(EditorAssetsTest, GetAssetInfoRefusesWhatIsNotAnAsset)
{
    EXPECT_EQ(Info("res://nothing.lua").type, ErrorType);
    EXPECT_EQ(Info("res://scripts").type, ErrorType) << "a folder";
    EXPECT_EQ(Info("res://../x.lua").type, ErrorType);
    EXPECT_EQ(Info("not a uuid or a path").type, ErrorType);
    EXPECT_EQ(Info("").type, ErrorType);
    EXPECT_EQ(Info(Math::UUID::Random().ToString()).type, ErrorType);

    WriteAsset("models/robot.mat", "{}");
    Rescan();
    const IO::ResourcePath robot("res://models/robot.mat");
    ASSERT_TRUE(Loader().SetSubAssetIndex(robot, {{"mesh/Arm", "Mesh", IO::ResourceUUID::FromSubAsset(robot, "mesh/Arm")}}));
    const Frame sub = Info(IO::ResourceUUID::FromSubAsset(robot, "mesh/Arm").ToString());
    EXPECT_EQ(sub.type, ErrorType);
    EXPECT_NE(sub.Text().find("res://models/robot.mat"), std::string::npos) << "it names the file: " << sub.Text();

    EditorServer noProject;
    EXPECT_EQ(Execute(noProject, CommandType::GetAssetInfo, Strings({"res://a.lua"})).type, ErrorType);
}

// ==================== SetImportSettings ====================

TEST_F(EditorAssetsTest, SetImportSettingsWritesTheMetaAndSaysSo)
{
    WriteAsset("a.mat", "{}");
    Rescan();
    const std::size_t eventsBefore = EventsOfKind(server, "assetsChanged").size();
    const std::uint32_t revisionBefore = server.GetFrameRevision();

    const json settings = {{"material", {{"note", "one"}}}, {"other", 2}};
    EXPECT_EQ(SetSettings("res://a.mat", settings).type, OkType);

    EXPECT_EQ(DecodeAssetDetail(Info("res://a.mat")).at("customData"), settings);
    EXPECT_EQ(json::parse(ReadFile(_root / ".import" / "a.mat.meta")).at("customData"), settings);
    const std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), eventsBefore + 1);
    EXPECT_EQ(StringList(events.back().at("modified")), (std::vector<std::string>{"res://a.mat"}));
    EXPECT_TRUE(events.back().at("added").empty());
    EXPECT_NE(server.GetFrameRevision(), revisionBefore) << "an import setting can change the picture";

    // A replacement, not a merge
    EXPECT_EQ(SetSettings("res://a.mat", json{{"other", 3}}).type, OkType);
    EXPECT_EQ(DecodeAssetDetail(Info("res://a.mat")).at("customData"), (json{{"other", 3}}));
    EXPECT_EQ(SetSettings("res://a.mat", json::object()).type, OkType);
    EXPECT_EQ(DecodeAssetDetail(Info("res://a.mat")).at("customData"), json::object());
}

TEST_F(EditorAssetsTest, SetImportSettingsDropsTheAssetMadeWithTheOldOnes)
{
    WriteAsset("a.mat", "{}");
    Rescan();
    const IO::ResourcePath path("res://a.mat");
    const auto held = std::make_shared<HeldAsset>();
    held->SetUUID(Loader().GetUUID(path));
    Loader().RegisterAsset(held, path);
    ASSERT_NE(Loader().GetCached<Base::Asset>(path), nullptr);

    EXPECT_EQ(SetSettings("res://a.mat", json{{"x", 1}}).type, OkType);

    EXPECT_EQ(Loader().GetCached<Base::Asset>(path), nullptr) << "the next use loads it with the new settings";
}

TEST_F(EditorAssetsTest, SetImportSettingsKeepsTheLoadersSubAssetIndexAndLeavesAScriptCached)
{
    const IO::ResourcePath script("res://scripts/Example.lua");
    const auto loaded = Loader().Load<LuaScript>(script);
    ASSERT_NE(loaded, nullptr);
    ASSERT_TRUE(Loader().SetSubAssetIndex(script, {{"mesh/Arm", "Mesh", IO::ResourceUUID::FromSubAsset(script, "mesh/Arm")}}));

    EXPECT_EQ(SetSettings("res://scripts/Example.lua", json{{"x", 1}, {"subAssets", {{"bogus", 1}}}}).type, OkType);

    const json info = DecodeAssetDetail(Info("res://scripts/Example.lua"));
    EXPECT_EQ(info.at("customData"), (json{{"x", 1}}));
    EXPECT_EQ(info.at("subAssets").at(0).at("key"), "mesh/Arm") << "the index is the loader's, not the client's";
    EXPECT_EQ(Loader().GetCached<LuaScript>(script), loaded) << "LuaComponents point at a script's object: it stays";
}

TEST_F(EditorAssetsTest, SetImportSettingsRefusesWhatItCannotApply)
{
    WriteAsset("a.mat", "{}");
    Rescan();
    const std::string before = ReadFile(_root / ".import" / "a.mat.meta");
    const std::size_t eventsBefore = EventsOfKind(server, "assetsChanged").size();

    EXPECT_EQ(SetSettings("res://a.mat", json::array()).type, ErrorType);
    EXPECT_EQ(SetSettings("res://a.mat", json("text")).type, ErrorType);
    EXPECT_EQ(SetSettings("res://a.mat", json(nullptr)).type, ErrorType);
    EXPECT_EQ(SetSettings("res://nothing.mat", json::object()).type, ErrorType);
    EXPECT_EQ(SetSettings("res://scenes", json::object()).type, ErrorType) << "a folder";
    EXPECT_EQ(SetSettings("res://../a.mat", json::object()).type, ErrorType);
    json large = json::object();
    large["text"] = std::string(EditorServer::MaxImportSettingsBytes, 'x');
    EXPECT_EQ(SetSettings("res://a.mat", large).type, ErrorType) << "over the size limit";

    EXPECT_EQ(ReadFile(_root / ".import" / "a.mat.meta"), before) << "nothing was applied";
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), eventsBefore);

    EditorServer noProject;
    BufferWriter request;
    request.WriteString("res://a.mat");
    WriteJson(request, json::object());
    EXPECT_EQ(Execute(noProject, CommandType::SetImportSettings, request.Release()).type, ErrorType);
}

// ==================== ReadTextAsset and WriteTextAsset ====================

TEST_F(EditorAssetsTest, ReadTextAssetReturnsTheFileAsItIs)
{
    EXPECT_EQ(DecodeTextData(Read("res://scripts/Example.lua")), ReadFile(Assets() / "scripts" / "Example.lua"));

    WriteAsset("notes.txt", "line one\r\nline two\r\n\xC3\xA9");
    EXPECT_EQ(DecodeTextData(Read("res://notes.txt")), "line one\r\nline two\r\n\xC3\xA9") << "line endings and UTF-8 kept";
    WriteAsset("bom.txt", "\xEF\xBB\xBFhello");
    EXPECT_EQ(DecodeTextData(Read("res://bom.txt")), "\xEF\xBB\xBFhello") << "a byte order mark is part of the text";
    WriteAsset("empty.json", "");
    EXPECT_EQ(DecodeTextData(Read("res://empty.json")), "");
    WriteAsset("UPPER.LUA", "return 1");
    EXPECT_EQ(DecodeTextData(Read("res://UPPER.LUA")), "return 1") << "an extension in any case";
}

TEST_F(EditorAssetsTest, ReadTextAssetRefusesWhatIsNotTextOrNotThere)
{
    WriteAsset("picture.png", "not really a picture");
    WriteAsset("data.bin", "abc");
    WriteAsset("bad.txt", "ok \xFF\xFE broken");
    WriteAsset("overlong.txt", "\xC0\x80");
    WriteAsset("surrogate.txt", "\xED\xA0\x80");
    WriteAsset("cut.txt", "ends in a cut char \xC3");
    WriteAsset("huge.txt", std::string(EditorServer::MaxTextAssetBytes + 1, 'a'));
    WriteAsset("exactly.txt", std::string(EditorServer::MaxTextAssetBytes, 'a'));

    for (const std::string &path : {"res://picture.png", "res://data.bin", "res://bad.txt", "res://overlong.txt",
                                    "res://surrogate.txt", "res://cut.txt", "res://huge.txt", "res://missing.txt",
                                    "res://scripts", "res://../x.txt", "res://scripts/Example.lua.meta", "notes.txt",
                                    "res://a:stream.txt", "res://nul.txt", "res://"})
    {
        EXPECT_EQ(Read(path).type, ErrorType) << path;
    }
    EXPECT_EQ(DecodeTextData(Read("res://exactly.txt")).size(), EditorServer::MaxTextAssetBytes) << "the limit is inclusive";

    EditorServer noProject;
    EXPECT_EQ(Execute(noProject, CommandType::ReadTextAsset, Strings({"res://a.txt"})).type, ErrorType);
}

TEST_F(EditorAssetsTest, WriteTextAssetWritesExactlyWhatItIsGivenAndIndexesTheFile)
{
    const std::string text = "return {}\r\n-- caf\xC3\xA9\r\n";
    EXPECT_EQ(Write("res://scripts/Player.lua", text).type, OkType);

    EXPECT_EQ(ReadFile(Assets() / "scripts" / "Player.lua"), text) << "no line ending added or changed";
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://scripts/Player.lua"))) << "indexed";
    EXPECT_TRUE(fs::exists(_root / ".import" / "scripts" / "Player.lua.meta"));
    for (const auto &entry : fs::directory_iterator(Assets() / "scripts"))
    {
        EXPECT_NE(entry.path().extension().string(), ".tmp")
            << "written through a temporary file, renamed: " << entry.path().string();
    }
    const std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(StringList(events.at(0).at("added")), (std::vector<std::string>{"res://scripts/Player.lua"}));
    EXPECT_EQ(DecodeTextData(Read("res://scripts/Player.lua")), text);

    // Written again, with the same length: still reported as modified
    EXPECT_EQ(Write("res://scripts/Player.lua", "return {}\r\n-- caf\xC3\xA9\r\n").type, OkType);
    EXPECT_EQ(Write("res://scripts/Player.lua", "return 2\r\n-- caf\xC3\xA9\r\n").type, OkType);
    const std::vector<json> after = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(after.size(), 3u);
    EXPECT_EQ(StringList(after.at(2).at("modified")), (std::vector<std::string>{"res://scripts/Player.lua"}));
    EXPECT_TRUE(after.at(2).at("added").empty());
}

TEST_F(EditorAssetsTest, WriteTextAssetOfAFileThatIsNoAssetSaysNothing)
{
    EXPECT_EQ(Write("res://notes/readme.txt", "x").type, ErrorType) << "its folder doesn't exist";
    EXPECT_EQ(Folder("res://notes").type, OkType);
    EXPECT_EQ(Write("res://notes/readme.txt", "hello").type, OkType);

    EXPECT_EQ(ReadFile(Assets() / "notes" / "readme.txt"), "hello");
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://notes/readme.txt"))) << "no loader for .txt: not an asset";
    EXPECT_TRUE(EventsOfKind(server, "assetsChanged").empty());
}

TEST_F(EditorAssetsTest, WriteTextAssetRefusesWhatItShouldNot)
{
    WriteAsset("scripts/Keep.lua", "keep me");
    const std::string keep = "keep me";

    EXPECT_EQ(Write("res://scripts/Keep.png", "x").type, ErrorType) << "a binary extension";
    EXPECT_EQ(Write("res://scripts/Keep.lua.meta", "x").type, ErrorType);
    EXPECT_EQ(Write("res://scripts/noextension", "x").type, ErrorType);
    EXPECT_EQ(Write("res://scripts/Keep.lua", std::string("a\0b", 3)).type, ErrorType) << "a NUL character";
    EXPECT_EQ(Write("res://scripts/Keep.lua", "bad \xFF utf8").type, ErrorType);
    EXPECT_EQ(Write("res://scripts/Keep.lua", std::string(EditorServer::MaxTextAssetBytes + 1, 'a')).type, ErrorType);
    EXPECT_EQ(Write("res://scripts", "x").type, ErrorType) << "a folder";
    EXPECT_EQ(Write("res://scripts/Sub.lua/x.lua", "x").type, ErrorType) << "the folder doesn't exist";
    EXPECT_EQ(Write("res://../outside.lua", "x").type, ErrorType);
    EXPECT_EQ(Write("res://scripts/../../outside.lua", "x").type, ErrorType);
    EXPECT_EQ(Write("res://scripts\\..\\..\\outside.lua", "x").type, ErrorType);
    EXPECT_EQ(Write("res:///scripts/Keep.lua:stream", "x").type, ErrorType) << "an NTFS stream";
    EXPECT_EQ(Write("C:/outside.lua", "x").type, ErrorType);
    EXPECT_EQ(Write("res://C:/outside.lua", "x").type, ErrorType);
    EXPECT_EQ(Write("/outside.lua", "x").type, ErrorType);
    EXPECT_EQ(Write("res://scripts/con.lua", "x").type, ErrorType) << "a Windows device name";
    EXPECT_EQ(Write("res://scripts/trailing.lua.", "x").type, ErrorType);
    EXPECT_EQ(Write("", "x").type, ErrorType);

    EXPECT_EQ(ReadFile(Assets() / "scripts" / "Keep.lua"), keep) << "nothing was written";
    EXPECT_FALSE(fs::exists(_base / "outside.lua"));
    EXPECT_FALSE(fs::exists(_root / "outside.lua"));
    EXPECT_TRUE(EventsOfKind(server, "assetsChanged").empty());

    EditorServer noProject;
    EXPECT_EQ(Execute(noProject, CommandType::WriteTextAsset, Strings({"res://a.txt", "x"})).type, ErrorType);
}

TEST_F(EditorAssetsTest, WriteTextAssetRefusesTheOpenScenesFile)
{
    ASSERT_NE(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Main.scene"})).type, ErrorType);
    const std::string before = ReadFile(Assets() / "scenes" / "Main.scene");

    EXPECT_EQ(Write("res://scenes/Main.scene", "{}").type, ErrorType);
    EXPECT_EQ(ReadFile(Assets() / "scenes" / "Main.scene"), before);

    // Another scene file is just a text file
    WriteAsset("scenes/Other.scene", R"({"name":"Other","rootGameObjects":[]})");
    Rescan();
    EXPECT_EQ(Write("res://scenes/Other.scene", R"({"name":"Other","rootGameObjects":[]} )").type, OkType);
}

TEST_F(EditorAssetsTest, TextAssetsKeepTheirPathsToTheProject)
{
    // A symlink inside assets/ that leads out of the project is refused for reads, writes, folders and listings
    const fs::path outside = _base / "outside-folder";
    fs::create_directories(outside);
    std::ofstream(outside / "secret.txt", std::ios::binary) << "secret";
    std::error_code error;
    fs::create_directory_symlink(outside, Assets() / "linked", error);
    if (error)
    {
        GTEST_SKIP() << "can't create a directory symlink here (" << error.message() << ")";
    }

    EXPECT_EQ(Read("res://linked/secret.txt").type, ErrorType);
    EXPECT_EQ(Write("res://linked/new.txt", "x").type, ErrorType);
    EXPECT_EQ(Write("res://linked/secret.txt", "x").type, ErrorType);
    EXPECT_EQ(Script("res://linked/Escape.lua", "").type, ErrorType);
    EXPECT_EQ(Folder("res://linked/sub").type, ErrorType);
    EXPECT_EQ(Info("res://linked/secret.txt").type, ErrorType);
    BufferWriter request;
    request.WriteString("res://linked");
    request.WriteBool(true);
    EXPECT_EQ(Execute(server, CommandType::ListAssets, request.Release()).type, ErrorType);

    EXPECT_EQ(ReadFile(outside / "secret.txt"), "secret");
    EXPECT_FALSE(fs::exists(outside / "new.txt"));
    EXPECT_FALSE(fs::exists(outside / "Escape.lua"));
    EXPECT_FALSE(fs::exists(outside / "sub"));
    // And a link in a listing is not followed
    const AssetListResult root = List("res://", true);
    for (const std::string &folder : root.folders)
        EXPECT_EQ(folder.find("linked"), std::string::npos) << folder;
}

TEST_F(EditorAssetsTest, ASymlinkedFileLeavingTheProjectIsRefused)
{
    const fs::path outside = _base / "outside.txt";
    std::ofstream(outside, std::ios::binary) << "secret";
    std::error_code error;
    fs::create_symlink(outside, Assets() / "link.txt", error);
    if (error)
    {
        GTEST_SKIP() << "can't create a symlink here (" << error.message() << ")";
    }

    EXPECT_EQ(Read("res://link.txt").type, ErrorType);
    EXPECT_EQ(Write("res://link.txt", "overwritten").type, ErrorType);
    EXPECT_EQ(ReadFile(outside), "secret");
}

// ==================== CreateScriptAsset ====================

TEST_F(EditorAssetsTest, CreateScriptAssetWritesTheTemplateAndIndexesIt)
{
    const Created created = DecodeAssetCreated(Script("res://scripts/enemies/Grunt.lua", "Grunt"));

    EXPECT_EQ(created.path, "res://scripts/enemies/Grunt.lua");
    EXPECT_EQ(created.uuid, IO::ResourceUUID::FromPath(IO::ResourcePath(created.path)).ToString());
    EXPECT_EQ(ReadFile(Assets() / "scripts" / "enemies" / "Grunt.lua"), MakeLuaScriptTemplate("Grunt"))
        << "the folder was made, and the file is the engine's template";
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath(created.path)));
    EXPECT_EQ(DecodeAssetDetail(Info(created.uuid)).at("type"), "LuaScript");
    const std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(StringList(events.at(0).at("added")), (std::vector<std::string>{created.path}));

    // An empty class name takes the file's name; a made-up one is made valid, as CreateScript does
    const Created named = DecodeAssetCreated(Script("res://scripts/my enemy.lua", ""));
    EXPECT_EQ(ReadFile(Assets() / "scripts" / "my enemy.lua"), MakeLuaScriptTemplate("my enemy"));
    EXPECT_NE(ReadFile(Assets() / "scripts" / "my enemy.lua").find("My_enemy"), std::string::npos);
    EXPECT_EQ(named.path, "res://scripts/my enemy.lua");
    const Created upper = DecodeAssetCreated(Script("res://scripts/Boss.LUA", "Boss"));
    EXPECT_EQ(upper.path, "res://scripts/Boss.LUA") << "an extension in any case";
}

TEST_F(EditorAssetsTest, CreateScriptAssetRefusesWhatItShouldNot)
{
    const std::string example = ReadFile(Assets() / "scripts" / "Example.lua");

    EXPECT_EQ(Script("res://scripts/Example.lua", "Other").type, ErrorType) << "it already exists";
    EXPECT_EQ(ReadFile(Assets() / "scripts" / "Example.lua"), example) << "and is left as it was";
    EXPECT_EQ(Script("res://scripts/Thing.txt", "").type, ErrorType) << "not a .lua path";
    EXPECT_EQ(Script("res://scripts/.lua", "").type, ErrorType) << "no name";
    EXPECT_EQ(Script("res://scripts", "").type, ErrorType);
    EXPECT_EQ(Script("res://../Escape.lua", "").type, ErrorType);
    EXPECT_EQ(Script("res://scripts/a:b.lua", "").type, ErrorType);
    EXPECT_EQ(Script("res://scripts/aux.lua", "").type, ErrorType);
    EXPECT_EQ(Script("scripts/Plain.lua", "").type, ErrorType) << "a res:// path is required";
    EXPECT_FALSE(fs::exists(_root / "Escape.lua"));
    EXPECT_TRUE(EventsOfKind(server, "assetsChanged").empty());

    EditorServer noProject;
    EXPECT_EQ(Execute(noProject, CommandType::CreateScriptAsset, Strings({"res://a.lua", ""})).type, ErrorType);
}

// ==================== CreateFolder ====================

TEST_F(EditorAssetsTest, CreateFolderMakesTheFoldersAboveItToo)
{
    EXPECT_EQ(Folder("res://levels/forest/trees").type, OkType);

    EXPECT_TRUE(fs::is_directory(Assets() / "levels" / "forest" / "trees"));
    const AssetListResult all = List("res://levels", true);
    EXPECT_EQ(all.folders, (std::vector<std::string>{"res://levels/forest", "res://levels/forest/trees"}));
    EXPECT_TRUE(EventsOfKind(server, "assetsChanged").empty()) << "an empty folder is no asset";

    EXPECT_EQ(Folder("res://levels/forest").type, OkType) << "already there: Ok";
    EXPECT_EQ(Folder("res://levels/forest/").type, OkType) << "a trailing slash";
}

TEST_F(EditorAssetsTest, CreateFolderRefusesWhatItShouldNot)
{
    EXPECT_EQ(Folder("res://scripts/Example.lua").type, ErrorType) << "a file is in the way";
    EXPECT_EQ(Folder("res://").type, ErrorType) << "the assets folder itself";
    EXPECT_EQ(Folder("").type, ErrorType);
    EXPECT_EQ(Folder("res://../outside").type, ErrorType);
    EXPECT_EQ(Folder("res://a/../../outside").type, ErrorType);
    EXPECT_EQ(Folder("res://bad:name").type, ErrorType);
    EXPECT_EQ(Folder("res://what?").type, ErrorType);
    EXPECT_EQ(Folder("res://nul").type, ErrorType);
    EXPECT_EQ(Folder("res://ends.with.dot.").type, ErrorType);
    EXPECT_EQ(Folder("levels").type, ErrorType) << "a res:// path is required";
    EXPECT_EQ(Folder(std::string("res://nul") + '\0' + "x").type, ErrorType);
    EXPECT_FALSE(fs::exists(_root / "outside"));
    EXPECT_FALSE(fs::exists(_base / "outside"));

    EditorServer noProject;
    EXPECT_EQ(Execute(noProject, CommandType::CreateFolder, Strings({"res://a"})).type, ErrorType);
}

// ==================== Path checks ====================

TEST(EditorAssetPathTest, NamesAreCheckedForEveryPlatform)
{
    for (const char *good : {"Player.lua", "a b", "caf\xC3\xA9.png", ".hidden", "a.b.c", "x", "COM10", "con_trol", "nullable.txt"})
        EXPECT_TRUE(EditorServer::IsValidAssetName(good)) << good;
    for (const char *bad : {"", ".", "..", "a:b", "a/b", "a\\b", "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "a\tb",
                            "trailing.", "trailing ", "CON", "con.txt", "Nul.lua", "AUX", "prn", "COM1", "lpt9.log",
                            "com1 .txt"})
        EXPECT_FALSE(EditorServer::IsValidAssetName(bad)) << bad;
    EXPECT_FALSE(EditorServer::IsValidAssetName(std::string(256, 'a')));
    EXPECT_TRUE(EditorServer::IsValidAssetName(std::string(255, 'a')));
    EXPECT_FALSE(EditorServer::IsValidAssetName(std::string("a\0b", 3)));
}

TEST(EditorAssetPathTest, TextExtensionsAreAnAllowList)
{
    for (const char *good : {"res://a.lua", "res://dir/a.MAT", "res://a.Scene", "res://x.json", "res://x.txt", "res://x.md",
                             "res://x.csv", "res://x.xml", "res://x.yaml", "res://x.yml", "res://x.toml", "res://x.ini",
                             "res://x.cfg", "res://x.glsl", "res://x.vert", "res://x.frag", "res://x.shader"})
        EXPECT_TRUE(EditorServer::IsTextAssetPath(good)) << good;
    for (const char *bad : {"res://a.png", "res://a.lua.meta", "res://a", "res://dir.lua/a", "res://a.", "res://.gitignore",
                            "res://a.exe", "res://a.glb", ""})
        EXPECT_FALSE(EditorServer::IsTextAssetPath(bad)) << bad;
}

TEST(EditorAssetPathTest, Utf8IsValidatedStrictly)
{
    EXPECT_TRUE(EditorServer::IsValidUtf8(""));
    EXPECT_TRUE(EditorServer::IsValidUtf8("plain ascii"));
    EXPECT_TRUE(EditorServer::IsValidUtf8("caf\xC3\xA9"));
    EXPECT_TRUE(EditorServer::IsValidUtf8("\xE2\x82\xAC"));         // the euro sign
    EXPECT_TRUE(EditorServer::IsValidUtf8("\xF0\x9F\x98\x80"));     // four bytes
    EXPECT_TRUE(EditorServer::IsValidUtf8("\xF4\x8F\xBF\xBF"));     // U+10FFFF
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xF4\x90\x80\x80"));    // past U+10FFFF
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xC0\x80"));            // overlong NUL
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xE0\x80\x80"));        // overlong
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xED\xA0\x80"));        // a surrogate
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xC3"));                // cut short
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xC3\x28"));            // a bad continuation
    EXPECT_FALSE(EditorServer::IsValidUtf8("\x80"));                // a continuation alone
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xFF"));
    EXPECT_FALSE(EditorServer::IsValidUtf8("\xF8\x88\x80\x80\x80")); // a five-byte form
}

TEST_F(EditorAssetsTest, ResolveAssetPathKeepsEverythingInsideTheAssetsFolder)
{
    const fs::path assets = Assets();
    const auto ok = EditorServer::ResolveAssetPath(assets, "res://scripts/New.lua");
    ASSERT_TRUE(ok) << ok.error();
    EXPECT_EQ(ok->resourcePath.ToString(), "res://scripts/New.lua");
    EXPECT_TRUE(ok->file == fs::weakly_canonical(assets) / "scripts" / "New.lua") << ok->file.string();

    EXPECT_FALSE(EditorServer::ResolveAssetPath(assets, "res://"));
    const auto root = EditorServer::ResolveAssetPath(assets, "res://", true);
    ASSERT_TRUE(root) << root.error();
    EXPECT_TRUE(root->file == fs::weakly_canonical(assets)) << root->file.string();
    EXPECT_EQ(root->resourcePath.ToString(), "res://");

    EXPECT_TRUE(EditorServer::ResolveAssetPath(assets, "res://scripts/./Example.lua")) << "a dot part is dropped";
    EXPECT_EQ(EditorServer::ResolveAssetPath(assets, "res://scripts//Example.lua")->resourcePath.ToString(),
              "res://scripts/Example.lua");
    EXPECT_FALSE(EditorServer::ResolveAssetPath(assets, "res://scripts/../../x"));
    EXPECT_FALSE(EditorServer::ResolveAssetPath(assets, "user://x"));
    EXPECT_FALSE(EditorServer::ResolveAssetPath(assets, "x"));
    EXPECT_FALSE(EditorServer::ResolveAssetPath(assets, std::string("res://a") + '\0'));
}

// ==================== Hot reload ====================

TEST_F(EditorAssetsTest, WritingALoadedScriptReloadsItInPlace)
{
    sol::state &lua = LuaRuntime::Instance().GetState();
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    lua["e8_loads"] = 0;
    lua["e8_version"] = 0;
    WriteAsset("scripts/Hot.lua", "e8_loads = e8_loads + 1\ne8_version = 1\nreturn {}\n");
    Rescan();
    const IO::ResourcePath path("res://scripts/Hot.lua");
    const auto script = Loader().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);
    // A component using the script: it is rebuilt when the script reloads
    const auto owner = GameObject::Create("Hot");
    auto *component = owner->AddComponent<LuaComponent>();
    component->SetScript(path);
    const int loadsBefore = lua["e8_loads"].get<int>();
    ASSERT_GT(loadsBefore, 0);

    EXPECT_EQ(Write("res://scripts/Hot.lua", "e8_loads = e8_loads + 1\ne8_version = 2\nreturn {}\n").type, OkType);

    EXPECT_EQ(lua["e8_version"].get<int>(), 2) << "the new source ran";
    EXPECT_EQ(lua["e8_loads"].get<int>(), loadsBefore + 2) << "once as the module, once for the component using it";
    EXPECT_EQ(Loader().GetCached<LuaScript>(path), script) << "the same object: components hold it";
    EXPECT_EQ(script->GetSourceCode(), "e8_loads = e8_loads + 1\ne8_version = 2\nreturn {}\n");
    const std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(StringList(events.back().at("modified")), (std::vector<std::string>{"res://scripts/Hot.lua"}));
    owner->RemoveComponent<LuaComponent>();
}

TEST_F(EditorAssetsTest, WritingAScriptNothingHoldsRunsNothing)
{
    sol::state &lua = LuaRuntime::Instance().GetState();
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    lua["e8_cold_runs"] = 0;
    WriteAsset("scripts/Cold.lua", "e8_cold_runs = e8_cold_runs + 1\nreturn {}\n");
    Rescan();
    ASSERT_EQ(Loader().GetCached<LuaScript>(IO::ResourcePath("res://scripts/Cold.lua")), nullptr);

    EXPECT_EQ(Write("res://scripts/Cold.lua", "e8_cold_runs = e8_cold_runs + 10\nreturn {}\n").type, OkType);

    EXPECT_EQ(lua["e8_cold_runs"].get<int>(), 0) << "a script that isn't loaded isn't run by being written";
}

TEST_F(EditorAssetsTest, AScriptErrorOnReloadIsALogEventNotAnErrorResponse)
{
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    WriteAsset("scripts/Broken.lua", "return {}\n");
    Rescan();
    const IO::ResourcePath path("res://scripts/Broken.lua");
    const auto script = Loader().Load<LuaScript>(path);
    ASSERT_NE(script, nullptr);
    const std::size_t logsBefore = EventsOfKind(server, "log").size();

    const Frame written = Write("res://scripts/Broken.lua", "this is not lua code (\n");

    EXPECT_EQ(written.type, OkType) << written.Text();
    EXPECT_EQ(ReadFile(Assets() / "scripts" / "Broken.lua"), "this is not lua code (\n") << "the write stays";
    bool errorLogged = false;
    const std::vector<json> logs = EventsOfKind(server, "log");
    for (std::size_t i = logsBefore; i < logs.size(); ++i)
    {
        errorLogged = errorLogged || (logs[i].value("level", "") == "error" &&
                                      logs[i].value("message", "").find("Broken") != std::string::npos);
    }
    EXPECT_TRUE(errorLogged) << "the runtime's error reached the log events";

    // Fixed: the next write reloads cleanly
    EXPECT_EQ(Write("res://scripts/Broken.lua", "return { fixed = true }\n").type, OkType);
    EXPECT_EQ(script->GetSourceCode(), "return { fixed = true }\n");
}

// ==================== The watcher ====================

TEST_F(EditorAssetsTest, PollAssetsFindsFilesChangedOutsideTheEditor)
{
    EXPECT_FALSE(server.PollAssets()) << "nothing changed";
    EXPECT_TRUE(EventsOfKind(server, "assetsChanged").empty());
    const std::uint32_t revision = server.GetFrameRevision();

    WriteAsset("scripts/Outside.lua", "return {}");
    WriteAsset("sub/b.mat", "{}");
    EXPECT_TRUE(server.PollAssets());
    EXPECT_FALSE(server.PollAssets()) << "once";
    std::vector<json> events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(StringList(events.at(0).at("added")), (std::vector<std::string>{"res://scripts/Outside.lua", "res://sub/b.mat"}));
    EXPECT_TRUE(events.at(0).at("removed").empty());
    EXPECT_TRUE(events.at(0).at("modified").empty());
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://sub/b.mat")));
    EXPECT_NE(server.GetFrameRevision(), revision) << "an asset the viewport shows may have changed";

    WriteAsset("sub/b.mat", R"({"edited": true})");
    EXPECT_TRUE(server.PollAssets());
    events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(StringList(events.at(1).at("modified")), (std::vector<std::string>{"res://sub/b.mat"}));

    fs::remove(Assets() / "scripts" / "Outside.lua");
    EXPECT_TRUE(server.PollAssets());
    events = EventsOfKind(server, "assetsChanged");
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(StringList(events.at(2).at("removed")), (std::vector<std::string>{"res://scripts/Outside.lua"}));
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://scripts/Outside.lua")));
}

TEST_F(EditorAssetsTest, TheWatcherSeesTwoEditsOfTheSameSizeEvenWhenTheyAreCloseTogether)
{
    WriteAsset("a.mat", R"({"v": 1})");
    ASSERT_TRUE(server.PollAssets());
    WriteAsset("a.mat", R"({"v": 2})");
    Touch("a.mat", std::chrono::milliseconds(100));
    EXPECT_TRUE(server.PollAssets()) << "same size, a tenth of a second apart";
    WriteAsset("a.mat", R"({"v": 3})");
    Touch("a.mat", std::chrono::milliseconds(300));
    EXPECT_TRUE(server.PollAssets());
}

TEST_F(EditorAssetsTest, TheWatcherReloadsWhatTheEngineHoldsAndLeavesTheRest)
{
    ASSERT_TRUE(LuaRuntime::Instance().Initialize());
    sol::state &lua = LuaRuntime::Instance().GetState();
    lua["e8_watch_runs"] = 0;
    WriteAsset("scripts/Watched.lua", "e8_watch_runs = e8_watch_runs + 1\nreturn {}\n");
    WriteAsset("held.mat", "{}");
    WriteAsset("unheld.mat", "{}");
    ASSERT_TRUE(server.PollAssets());
    const IO::ResourcePath script("res://scripts/Watched.lua");
    const IO::ResourcePath held("res://held.mat");
    const auto loaded = Loader().Load<LuaScript>(script);
    ASSERT_NE(loaded, nullptr);
    const auto heldAsset = std::make_shared<HeldAsset>();
    heldAsset->SetUUID(Loader().GetUUID(held));
    Loader().RegisterAsset(heldAsset, held);

    WriteAsset("scripts/Watched.lua", "e8_watch_runs = e8_watch_runs + 100\nreturn {}\n");
    WriteAsset("held.mat", R"({"changed": true})");
    WriteAsset("unheld.mat", R"({"changed": true})");
    ASSERT_TRUE(server.PollAssets());

    EXPECT_EQ(lua["e8_watch_runs"].get<int>(), 100) << "the held script ran its new source (the old one never ran)";
    EXPECT_EQ(loaded->GetSourceCode(), "e8_watch_runs = e8_watch_runs + 100\nreturn {}\n");
    EXPECT_EQ(Loader().GetCached<LuaScript>(script), loaded) << "reloaded in place";
    EXPECT_EQ(Loader().GetCached<Base::Asset>(held), nullptr) << "a held asset is dropped, so its next use reads the file";
}

TEST_F(EditorAssetsTest, TheWatcherWaitsForAClientAndTheInterval)
{
    auto now = std::chrono::steady_clock::now();
    server.SetAssetPollClock([&now] { return now; });
    server.SetAssetPollInterval(std::chrono::milliseconds(1000));
    EXPECT_EQ(server.GetAssetPollInterval(), std::chrono::milliseconds(1000));
    EXPECT_TRUE(server.IsWatchingAssets());
    EXPECT_FALSE(server.HasClient());

    WriteAsset("a.mat", "{}");
    EXPECT_FALSE(server.PollAssetsIfDue()) << "no client connected: the folder is not even looked at";
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://a.mat")));

    server.SetClientConnected(true);
    EXPECT_TRUE(server.HasClient());
    EXPECT_TRUE(server.PollAssetsIfDue()) << "the first check, as soon as there is a client";
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://a.mat")));

    WriteAsset("b.mat", "{}");
    now += std::chrono::milliseconds(999);
    EXPECT_FALSE(server.PollAssetsIfDue()) << "the interval hasn't passed";
    EXPECT_FALSE(Loader().Exists(IO::ResourcePath("res://b.mat")));
    now += std::chrono::milliseconds(1);
    EXPECT_TRUE(server.PollAssetsIfDue());
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://b.mat")));

    server.SetAssetWatching(false);
    EXPECT_FALSE(server.IsWatchingAssets());
    WriteAsset("c.mat", "{}");
    now += std::chrono::seconds(10);
    EXPECT_FALSE(server.PollAssetsIfDue()) << "switched off";
    server.SetAssetWatching(true);
    server.SetClientConnected(false);
    EXPECT_FALSE(server.PollAssetsIfDue()) << "the client left";
    server.SetClientConnected(true);
    EXPECT_TRUE(server.PollAssetsIfDue());
    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://c.mat")));
}

TEST_F(EditorAssetsTest, ProcessCommandsRunsTheWatcher)
{
    server.SetClientConnected(true);
    WriteAsset("a.mat", "{}");

    (void)server.ProcessCommands();

    EXPECT_TRUE(Loader().Exists(IO::ResourcePath("res://a.mat")));
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), 1u);

    // An idle check changes nothing, and costs no event
    (void)server.ProcessCommands();
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), 1u);
}

TEST_F(EditorAssetsTest, TheWatcherDoesNotRepeatWhatACommandAlreadyReported)
{
    EXPECT_EQ(Write("res://scripts/Mine.lua", "return {}").type, OkType);
    ASSERT_EQ(EventsOfKind(server, "assetsChanged").size(), 1u);

    EXPECT_FALSE(server.PollAssets()) << "the command already indexed the file: the rescan finds nothing new";
    EXPECT_EQ(EventsOfKind(server, "assetsChanged").size(), 1u);
}

TEST_F(EditorAssetsTest, RescanAssetsReloadsAModifiedHeldAsset)
{
    WriteAsset("held.mat", "{}");
    Rescan();
    const IO::ResourcePath held("res://held.mat");
    const auto asset = std::make_shared<HeldAsset>();
    asset->SetUUID(Loader().GetUUID(held));
    Loader().RegisterAsset(asset, held);

    WriteAsset("held.mat", R"({"changed": true})");
    Rescan();

    EXPECT_EQ(Loader().GetCached<Base::Asset>(held), nullptr);
}

TEST_F(EditorAssetsTest, ThePollerWithoutAProjectDoesNothing)
{
    EditorServer noProject;
    noProject.SetClientConnected(true);
    EXPECT_FALSE(noProject.PollAssets());
    EXPECT_FALSE(noProject.PollAssetsIfDue());
}
