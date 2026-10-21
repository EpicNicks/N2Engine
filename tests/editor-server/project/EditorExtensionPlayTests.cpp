#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
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

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// The Lua editor extension commands on a PLAY host (protocol 1.14.0), really executed through ExecuteCommand: a play
// host plays a snapshot and runs no extension, so ReloadEditorExtensions is refused there (the edit-only gate), while
// the two reads still answer. This is its own program (editor_project_tests) because a play host loads a scene into
// SceneManager and needs a project; the role, read-only and flag cases are in EditorExtensionCommandsTests.cpp.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t PlaySnapshotType = static_cast<uint8_t>(ResponseType::PlaySnapshot);
    constexpr uint8_t EditorExtensionsType = static_cast<uint8_t>(ResponseType::EditorExtensions);
    constexpr uint8_t EditorUiType = static_cast<uint8_t>(ResponseType::EditorUi);

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

    // The JSON a response of one json field carries
    json DecodeJson(const Frame &frame, const uint8_t expectedType)
    {
        EXPECT_EQ(frame.type, expectedType) << frame.Text();
        if (frame.type != expectedType)
            return json();
        BufferReader r(frame.payload);
        const std::string text = r.ReadString();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the json field";
        return json::parse(text);
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

    std::string ExtensionText(const std::string &name, const std::string &itemId)
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = { \"scene.read\" },\n"
               "  menu = { { id = \"" + itemId + "\", path = \"Tools/" + name + "\", run = function() end } },\n"
               "}\n";
    }

    // A project with a saved (empty) scene and a file in its editor/ folder, an edit host, and a play host made from a
    // snapshot of that scene
    class EditorExtensionPlayTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-extension-play-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "Ext Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "Ext Game");
            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));

            WriteExtension("a.lua", ExtensionText("A", "a.item"));

            edit.SetProject(_root, _project);
            const Frame made = Execute(edit, CommandType::NewScene, Strings({"res://scenes/Play.scene", "Play Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            const Frame snapshot = Execute(edit, CommandType::WritePlaySnapshot, Strings({""}));
            ASSERT_EQ(snapshot.type, PlaySnapshotType) << snapshot.Text();
            BufferReader r(snapshot.payload);
            const std::string file = r.ReadString();
            _snapshot = fs::path(std::u8string(file.begin(), file.end()));
        }

        void TearDown() override
        {
            play.reset();
            IO::ResourceLoader::Instance().SetReadOnly(false);
            (void)Execute(edit, CommandType::NewScene, Strings({"", "Empty"}));
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        void WriteExtension(const std::string &name, const std::string &text) const
        {
            const fs::path path = _root / "editor" / name;
            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

        // The play host; `withExtensions` makes it one started with --editor-extensions (before it plays: enabling is
        // refused on a host that is playing)
        void StartPlaying(const bool withExtensions)
        {
            play = std::make_unique<EditorServer>();
            if (withExtensions)
            {
                ASSERT_TRUE(play->EnableEditorExtensions(_root));
            }
            const auto entered = play->EnterPlayMode(_snapshot);
            ASSERT_TRUE(entered) << entered.error();
            ASSERT_TRUE(play->IsPlayMode());
        }

        EditorServer edit;
        std::unique_ptr<EditorServer> play;

    private:
        fs::path _base;
        fs::path _root;
        fs::path _snapshot;
        IO::ProjectFile _project;
    };
}

TEST_F(EditorExtensionPlayTest, APlayHostRefusesTheReloadAndStillAnswersTheTwoReads)
{
    StartPlaying(true);
    ASSERT_NE(play->GetEditorExtensions(), nullptr);
    ASSERT_EQ(play->GetEditorExtensions()->LoadedCount(), 1u);

    const json list = DecodeJson(Execute(*play, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_TRUE(list.at("enabled").get<bool>());
    EXPECT_EQ(list.at("uiRevision"), 1);
    ASSERT_EQ(list.at("extensions").size(), 1u);
    EXPECT_EQ(list.at("extensions")[0].at("state"), "loaded");
    const json ui = DecodeJson(Execute(*play, CommandType::GetEditorUi), EditorUiType);
    ASSERT_EQ(ui.at("menuItems").size(), 1u);

    // A file added after the host started: a reload would pick it up, the refusal must not
    WriteExtension("b.lua", ExtensionText("B", "b.item"));
    const Frame refused = Execute(*play, CommandType::ReloadEditorExtensions);
    EXPECT_EQ(refused.type, ErrorType) << refused.Text();
    EXPECT_NE(refused.Text().find("Not available in a play host"), std::string::npos) << refused.Text();

    EXPECT_EQ(play->GetEditorExtensions()->ExtensionCount(), 1u) << "no file was read again";
    EXPECT_EQ(play->GetEditorExtensions()->UiRevision(), 1u);
    EXPECT_TRUE(EventsOfKind(*play, "extensionsChanged").empty());
    EXPECT_EQ(DecodeJson(Execute(*play, CommandType::ListEditorExtensions), EditorExtensionsType)
                  .at("extensions").size(), 1u);
}

TEST_F(EditorExtensionPlayTest, APlayHostRefusesInvokeMenuItemBeforeAnyLuaRuns)
{
    StartPlaying(true);
    ASSERT_NE(play->GetEditorExtensions(), nullptr);
    ASSERT_EQ(play->GetEditorExtensions()->LoadedCount(), 1u);
    BufferWriter w;
    w.WriteString("a.item");
    WriteJson(w, json::array());
    WriteJson(w, json::object());
    w.WriteU32(play->GetEditorExtensions()->UiRevision());

    // InvokeMenuItem is edit-only (since 1.15.0): a play host runs no extension, so the play gate answers first
    const Frame refused = Execute(*play, CommandType::InvokeMenuItem, w.Release());
    EXPECT_EQ(refused.type, ErrorType) << refused.Text();
    EXPECT_NE(refused.Text().find("Not available in a play host"), std::string::npos) << refused.Text();
    EXPECT_TRUE(EventsOfKind(*play, "extensionOutput").empty());
    EXPECT_FALSE(play->GetHistory().InGroup());
    EXPECT_EQ(play->GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded) << "nothing ran or failed";
}

TEST_F(EditorExtensionPlayTest, APlayHostWithoutTheFlagAnswersDisabledAndRefusesTheReloadAsAPlayHost)
{
    StartPlaying(false);
    EXPECT_EQ(play->GetEditorExtensions(), nullptr);

    const json list = DecodeJson(Execute(*play, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_FALSE(list.at("enabled").get<bool>());
    EXPECT_TRUE(list.at("extensions").empty());
    const json ui = DecodeJson(Execute(*play, CommandType::GetEditorUi), EditorUiType);
    EXPECT_TRUE(ui.at("menuItems").empty());

    // The play gate is checked before the command's own handler, so it speaks first
    const Frame refused = Execute(*play, CommandType::ReloadEditorExtensions);
    EXPECT_EQ(refused.type, ErrorType) << refused.Text();
    EXPECT_NE(refused.Text().find("Not available in a play host"), std::string::npos) << refused.Text();
}
