#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/ExtensionHost.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/scripting/LuaSandbox.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Lua editor extensions against a real project folder (IO::CreateProject): no scene path loads or reloads an extension
// (design 6.5), and the project's editor/ folder cannot be reached through the asset commands that an editor-role user
// has (design 5.1). Part of the editor_project_tests program, which owns ResourceLoader and SceneManager for its tests.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EditorExtensionsType = static_cast<uint8_t>(ResponseType::EditorExtensions);
    constexpr uint8_t AssetListType = static_cast<uint8_t>(ResponseType::AssetList);

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return std::string(payload.begin(), payload.end()); }
    };

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(static_cast<uint8_t>(type), payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const std::span<const uint8_t> body = r.ReadBytes(r.ReadU32());
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

    json ListExtensions(EditorServer &server)
    {
        const Frame frame = Execute(server, CommandType::ListEditorExtensions);
        EXPECT_EQ(frame.type, EditorExtensionsType) << frame.Text();
        BufferReader r(frame.payload);
        return json::parse(r.ReadString());
    }

    // How many extensionsChanged events the server has pushed
    size_t ExtensionsChangedEvents(const EditorServer &server)
    {
        size_t count = 0;
        for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
        {
            count += event.value("kind", "") == "extensionsChanged" ? 1 : 0;
        }
        return count;
    }

    std::string ExtensionText(const std::string &name)
    {
        return "return Editor.Extension { name = \"" + name + "\", apiVersion = 1 }\n";
    }

    class EditorExtensionsSceneTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-extensions-scene-test" / info->name();
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

        void WriteExtension(const std::string &relative, const std::string &text) const
        {
            const fs::path path = _root / "editor" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        }

        // Every way a scene is made, loaded or replaced: none of them may touch an extension
        void ExerciseEveryScenePath()
        {
            EXPECT_EQ(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Main.scene"})).type, SceneInfoType);
            EXPECT_EQ(Execute(server, CommandType::NewScene, Strings({"res://scenes/Other.scene", "Other"})).type,
                      SceneInfoType);
            const json scene = {{"name", "Loaded"}, {"rootGameObjects", json::array()}};
            EXPECT_NE(Execute(server, CommandType::LoadScene, Strings({scene.dump()})).type, ErrorType);
            EXPECT_EQ(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Main.scene"})).type, SceneInfoType);
            EXPECT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
        }

        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        EditorServer server;
    };
}

TEST_F(EditorExtensionsSceneTest, NoScenePathLoadsAnExtensionOnAHostWithoutTheFlag)
{
    WriteExtension("Ext.lua", ExtensionText("Ext"));
    const int statesBefore = Scripting::LuaSandbox::LiveStates();

    ExerciseEveryScenePath();

    const json list = ListExtensions(server);
    EXPECT_FALSE(list.at("enabled").get<bool>());
    EXPECT_TRUE(list.at("extensions").empty());
    EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), statesBefore) << "no extension state was made";
}

TEST_F(EditorExtensionsSceneTest, NoScenePathReloadsAnExtensionOnAHostWithTheFlag)
{
    WriteExtension("Ext.lua", ExtensionText("Before"));
    ASSERT_TRUE(server.EnableEditorExtensions(_root));
    const json first = ListExtensions(server);
    ASSERT_EQ(first.at("extensions").size(), 1u);
    EXPECT_EQ(first.at("extensions")[0].at("name"), "Before");
    const int statesBefore = Scripting::LuaSandbox::LiveStates();

    // The file changes on disk, then every scene path runs: the registry is what was loaded at start
    WriteExtension("Ext.lua", ExtensionText("After"));
    WriteExtension("Added.lua", ExtensionText("Added"));
    ExerciseEveryScenePath();
    EXPECT_EQ(ListExtensions(server), first);
    EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), statesBefore);
    EXPECT_EQ(ExtensionsChangedEvents(server), 0u) << "no scene path says the extensions changed";

    // Only the explicit reload reads them again
    const Frame reloaded = Execute(server, CommandType::ReloadEditorExtensions);
    ASSERT_EQ(reloaded.type, EditorExtensionsType) << reloaded.Text();
    const json second = ListExtensions(server);
    EXPECT_EQ(second.at("uiRevision"), 2);
    ASSERT_EQ(second.at("extensions").size(), 2u);
    EXPECT_EQ(second.at("extensions")[0].at("name"), "Added");
    EXPECT_EQ(second.at("extensions")[1].at("name"), "After");
}

TEST_F(EditorExtensionsSceneTest, TheEditorFolderIsOutsideWhatTheAssetCommandsReach)
{
    WriteExtension("Ext.lua", ExtensionText("Ext"));
    ASSERT_TRUE(server.EnableEditorExtensions(_root));

    // Not listed as an asset
    BufferWriter listing;
    listing.WriteString("res://");
    listing.WriteBool(true);
    const Frame assets = Execute(server, CommandType::ListAssets, listing.Release());
    ASSERT_EQ(assets.type, AssetListType) << assets.Text();
    EXPECT_EQ(assets.Text().find("Ext.lua"), std::string::npos) << "a viewer's asset list does not show the extension files";
    EXPECT_EQ(assets.Text().find("editor"), std::string::npos);

    // Not readable, not writable, not creatable through a res:// path, whatever the spelling
    for (const std::string path : {"res://../editor/Ext.lua", "res://scenes/../../editor/Ext.lua", "res://..\\editor\\Ext.lua",
                                   "../editor/Ext.lua", "editor/Ext.lua"})
    {
        EXPECT_EQ(Execute(server, CommandType::ReadTextAsset, Strings({path})).type, ErrorType) << path;
        EXPECT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({path, "return nil"})).type, ErrorType) << path;
    }
    EXPECT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://../editor/Evil.lua", ExtensionText("Evil")})).type,
              ErrorType);
    EXPECT_EQ(Execute(server, CommandType::CreateScriptAsset, Strings({"res://../editor/Evil2.lua", "Evil2"})).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::CreateFolder, Strings({"res://../editor/sub"})).type, ErrorType);
    EXPECT_FALSE(fs::exists(_root / "editor" / "Evil.lua"));
    EXPECT_FALSE(fs::exists(_root / "editor" / "Evil2.lua"));
    EXPECT_FALSE(fs::exists(_root / "editor" / "sub"));

    // What WriteTextAsset can write is under assets/, even in a folder that is called editor: not an extension
    EXPECT_EQ(Execute(server, CommandType::CreateFolder, Strings({"res://editor"})).type, OkType);
    EXPECT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://editor/Planted.lua", ExtensionText("Planted")})).type,
              OkType);
    EXPECT_TRUE(fs::exists(_root / "assets" / "editor" / "Planted.lua"));
    ASSERT_EQ(Execute(server, CommandType::ReloadEditorExtensions).type, EditorExtensionsType);
    const json list = ListExtensions(server);
    ASSERT_EQ(list.at("extensions").size(), 1u) << list.dump();
    EXPECT_EQ(list.at("extensions")[0].at("id"), "Ext.lua") << "a script an editor wrote is never an extension";
}
