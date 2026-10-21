#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
#include <editor-server/EventRing.hpp>
#include <editor-server/ExtensionHost.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>
#include <editor-server/TokenTable.hpp>
#include <engine/Logger.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// Where the host calls the hooks of a Lua editor extension, and Editor.Assets (E-X6), against a project with a real scene,
// really executed through ExecuteCommand: OnSceneOpened after OpenScene and NewScene, OnSceneSaved after the file is written
// and the history marked saved, OnBeforePlay before the play snapshot (its warnings are events, play is never blocked),
// OnAssetsChanged from a rescan, a client's write and the watcher's poll; a hook that fails changes nothing about the
// triggering command; an extension's own asset write does not call the hook. Editor.Assets reads and writes text assets
// through the client's own commands: res:// only, traversal and links refused, size caps, capabilities, the audit line and
// the role table. Part of the editor_project_tests program, which owns SceneManager and ResourceLoader for its tests.
// The hooks and the API without a project are in ../ExtensionHooksTests.cpp.
namespace
{
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t PlaySnapshotType = static_cast<uint8_t>(ResponseType::PlaySnapshot);
    constexpr uint8_t ExtensionResultType = static_cast<uint8_t>(ResponseType::ExtensionResult);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);

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

    Frame Parse(const std::vector<uint8_t> &frame)
    {
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const std::span<const uint8_t> body = r.ReadBytes(r.ReadU32());
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

    std::vector<uint8_t> HelloWithErrorCodes(const std::string &token = "")
    {
        BufferWriter w;
        w.WriteString("hooks scene test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(token);
        w.WriteBool(true);
        return w.Release();
    }

    struct Coded
    {
        std::string message;
        std::string code;
    };

    Coded DecodeErrorCoded(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ErrorCodedType) << frame.Text();
        Coded coded;
        if (frame.type != ErrorCodedType)
            return coded;
        BufferReader r(frame.payload);
        coded.message = r.ReadString();
        coded.code = r.ReadString();
        return coded;
    }

    // The message an item returned
    std::string ResultMessage(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ExtensionResultType) << frame.Text();
        if (frame.type != ExtensionResultType)
            return {};
        BufferReader r(frame.payload);
        (void)r.ReadString(); // label
        (void)r.ReadU32();    // revision
        (void)r.ReadBool();   // canUndo
        (void)r.ReadBool();   // canRedo
        (void)r.ReadU32();    // savedRevision
        const json result = ReadJson(r);
        return result.value("message", "");
    }

    struct SceneInfo
    {
        std::string path;
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
        (void)r.ReadString(); // name
        (void)r.ReadString(); // uuid
        info.revision = r.ReadU32();
        info.savedRevision = r.ReadU32();
        return info;
    }

    std::vector<json> EventsOfKind(const EditorServer &server, const std::string &kind,
                                   const EventView view = EventView::Full)
    {
        std::vector<json> found;
        for (const json &event : server.GetEvents().Read(0, 4096, 0, view).events)
        {
            if (event.value("kind", "") == kind)
                found.push_back(event);
        }
        return found;
    }

    // The messages of the extensionOutput events, oldest first
    std::vector<std::string> Messages(const EditorServer &server, const EventView view = EventView::Full)
    {
        std::vector<std::string> messages;
        for (const json &event : EventsOfKind(server, "extensionOutput", view))
            messages.push_back(event.value("message", ""));
        return messages;
    }

    class LogCapture
    {
    public:
        LogCapture()
        {
            _id = Logger::logEvent += [this](const std::string_view message, Logger::LogLevel)
            {
                const std::lock_guard lock(_mutex);
                _lines.emplace_back(message);
            };
        }

        ~LogCapture() { Logger::logEvent -= _id; }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] bool HasLine(const std::string_view first, const std::string_view second) const
        {
            const std::lock_guard lock(_mutex);
            for (const std::string &line : _lines)
            {
                if (line.find(first) != std::string::npos && line.find(second) != std::string::npos)
                    return true;
            }
            return false;
        }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        std::size_t _id = 0;
    };

    // An extension with these capabilities (the Lua list's inside) and these hooks (the inside of the `hooks` table)
    std::string HookExtension(const std::string &name, const std::string &capabilities, const std::string &hooks,
                              const std::string &menu = "")
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = { " + capabilities + " },\n"
               "  hooks = {\n" + hooks + "\n  },\n"
               "  menu = {\n" + menu + "\n  },\n"
               "}\n";
    }

    // Items that use Editor.Assets, with a path and a text as their form
    const std::string AssetItems =
        "    { id = 'a.read', path = 'Tools/Read', params = { path = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p) return { message = Editor.Assets.ReadText(p.path) } end },\n"
        "    { id = 'a.write', path = 'Tools/Write',\n"
        "      params = { path = { type = 'string', default = '' }, text = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p) Editor.Assets.WriteText(p.path, p.text) return { message = 'written' } end },\n"
        "    { id = 'a.folder', path = 'Tools/Folder', params = { path = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p) Editor.Assets.CreateFolder(p.path) return { message = 'made' } end },\n"
        "    { id = 'a.list', path = 'Tools/List', params = { path = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p)\n"
        "        local l = Editor.Assets.List(p.path)\n"
        "        local names = {}\n"
        "        for _, f in ipairs(l.folders) do names[#names + 1] = f end\n"
        "        for _, a in ipairs(l.assets) do names[#names + 1] = a.path end\n"
        "        table.sort(names)\n"
        "        return { message = table.concat(names, ',') }\n"
        "      end },\n"
        "    { id = 'a.big', path = 'Tools/Big',\n"
        "      run = function(ctx, p) Editor.Assets.WriteText('res://big.txt', string.rep('x', 4 * 1024 * 1024 + 1)) end },\n"
        // The same calls with the refusal caught, so a test can make many of them without the extension being disabled for
        // failing too often: the message is "ok" or "error: <what the host said>"
        "    { id = 't.read', path = 'Tools/TryRead', params = { path = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p)\n"
        "        local ok, e = pcall(Editor.Assets.ReadText, p.path)\n"
        "        if ok then return { message = 'ok' } end\n"
        "        return { message = 'error: ' .. tostring(e) }\n"
        "      end },\n"
        "    { id = 't.write', path = 'Tools/TryWrite',\n"
        "      params = { path = { type = 'string', default = '' }, text = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p)\n"
        "        local ok, e = pcall(Editor.Assets.WriteText, p.path, p.text)\n"
        "        if ok then return { message = 'ok' } end\n"
        "        return { message = 'error: ' .. tostring(e) }\n"
        "      end },\n"
        "    { id = 't.folder', path = 'Tools/TryFolder', params = { path = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p)\n"
        "        local ok, e = pcall(Editor.Assets.CreateFolder, p.path)\n"
        "        if ok then return { message = 'ok' } end\n"
        "        return { message = 'error: ' .. tostring(e) }\n"
        "      end },\n"
        "    { id = 't.list', path = 'Tools/TryList', params = { path = { type = 'string', default = '' } },\n"
        "      run = function(ctx, p)\n"
        "        local ok, e = pcall(Editor.Assets.List, p.path)\n"
        "        if ok then return { message = 'ok' } end\n"
        "        return { message = 'error: ' .. tostring(e) }\n"
        "      end },\n"
        "    { id = 't.big', path = 'Tools/TryBig',\n"
        "      run = function(ctx, p)\n"
        "        local ok, e = pcall(Editor.Assets.WriteText, 'res://big.txt', string.rep('x', 4 * 1024 * 1024 + 1))\n"
        "        if ok then return { message = 'ok' } end\n"
        "        return { message = 'error: ' .. tostring(e) }\n"
        "      end },\n";

    class EditorExtensionHooksTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _watchdog.emplace(std::chrono::seconds(120));
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-extension-hooks-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "Hooks Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "Hooks Game");
            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));
            server.SetProject(_root, _project);
            // The tests poll the assets themselves
            server.SetAssetPollInterval(std::chrono::milliseconds(0));

            // A saved scene to open and save again; made before the extensions are on, so no hook has seen it
            const Frame made = Execute(server, CommandType::NewScene, Strings({"res://scenes/Hooks.scene", "Hooks"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            ASSERT_EQ(Execute(server, CommandType::SaveSceneToFile, Strings({""})).type, SceneInfoType);
        }

        void TearDown() override
        {
            ExtensionHost::SetHookBudgetForTest(0, 0);
            IO::ResourceLoader::Instance().SetReadOnly(false);
            (void)server.SetReadOnly(false);
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            (void)server.ShutdownEditorExtensions(std::chrono::milliseconds(2000));
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
            _watchdog.reset();
        }

        void WriteExtension(const std::string &name, const std::string &text) const
        {
            const fs::path path = _root / "editor" / name;
            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

        // The extensions on and a Hello (admin: a host with no access token has no roles)
        void Start()
        {
            ASSERT_TRUE(server.EnableEditorExtensions(_root));
            ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
        }

        [[nodiscard]] fs::path Assets() const { return _root / "assets"; }

        void WriteAsset(const std::string &relative, const std::string &text) const
        {
            const fs::path path = Assets() / relative;
            std::error_code error;
            fs::create_directories(path.parent_path(), error);
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        }

        [[nodiscard]] static std::string ReadFile(const fs::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }

        [[nodiscard]] std::string ReadAsset(const std::string &relative) const { return ReadFile(Assets() / relative); }

        Frame Invoke(const std::string &itemId, const json &params = json::object())
        {
            BufferWriter w;
            w.WriteString(itemId);
            WriteJson(w, json::array());
            WriteJson(w, params);
            w.WriteU32(server.GetEditorExtensions()->UiRevision());
            return Execute(server, CommandType::InvokeMenuItem, w.Release());
        }

        EditorServer server;

    private:
        std::optional<Tests::HangWatchdog> _watchdog;
        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;

    protected:
        [[nodiscard]] const fs::path &Root() const { return _root; }
    };
}

// ==================== OnSceneOpened ====================

TEST_F(EditorExtensionHooksTest, OnSceneOpenedRunsAfterTheSceneIsLoadedAndCannotFailTheOpen)
{
    WriteExtension("a.lua", HookExtension("Opened", "\"scene.read\"",
                                          "    OnSceneOpened = function(info)\n"
                                          "      -- the scene is the open one already: its hierarchy can be read\n"
                                          "      local h = Editor.Scene.GetHierarchy()\n"
                                          "      Editor.Log.Info('opened ' .. info.scene .. ' ' .. type(h))\n"
                                          "    end,"));
    WriteExtension("b.lua", HookExtension("Broken", "\"scene.read\"",
                                          "    OnSceneOpened = function(info) error('boom') end,"));
    Start();

    const Frame opened = Execute(server, CommandType::OpenScene, Strings({"res://scenes/Hooks.scene"}));
    EXPECT_EQ(DecodeSceneInfo(opened).path, "res://scenes/Hooks.scene") << "a hook that errors does not fail the open";
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"opened res://scenes/Hooks.scene table"}));
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("b.lua"), ExtensionState::Loaded) << "one failure";
}

TEST_F(EditorExtensionHooksTest, OnSceneOpenedRunsForANewSceneWithoutAFileToo)
{
    WriteExtension("a.lua", HookExtension("Opened", "\"scene.read\"",
                                          "    OnSceneOpened = function(info) Editor.Log.Info('opened [' .. info.scene .. ']') end,"));
    Start();
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Fresh"})).type, SceneInfoType);
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"res://scenes/Fresh.scene", "Fresh"})).type, SceneInfoType);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"opened []", "opened [res://scenes/Fresh.scene]"}));
}

TEST_F(EditorExtensionHooksTest, ASceneThatFailsToOpenCallsNoHook)
{
    WriteExtension("a.lua", HookExtension("Opened", "\"scene.read\"",
                                          "    OnSceneOpened = function(info) Editor.Log.Info('opened') end,"));
    Start();
    EXPECT_NE(Execute(server, CommandType::OpenScene, Strings({"res://scenes/Missing.scene"})).type, SceneInfoType);
    EXPECT_TRUE(Messages(server).empty());
}

// ==================== OnSceneSaved ====================

TEST_F(EditorExtensionHooksTest, OnSceneSavedRunsAfterTheFileIsWrittenAndTheSceneMarkedSaved)
{
    WriteExtension("a.lua", HookExtension("Saved", "\"scene.read\", \"assets.read\"",
                                          "    OnSceneSaved = function(info)\n"
                                          "      -- the file is written and indexed: the assets of its folder list it\n"
                                          "      local l = Editor.Assets.List('res://scenes')\n"
                                          "      local names = {}\n"
                                          "      for _, a in ipairs(l.assets) do names[#names + 1] = a.path end\n"
                                          "      Editor.Log.Info('saved ' .. info.scene .. ' in ' .. table.concat(names, ','))\n"
                                          "    end,"));
    WriteExtension("b.lua", HookExtension("Broken", "\"scene.read\"",
                                          "    OnSceneSaved = function(info) error('boom') end,"));
    Start();

    BufferWriter entity;
    entity.WriteString("Cube");
    entity.WriteString("");
    entity.WriteI32(-1);
    entity.WriteString("");
    ASSERT_EQ(Execute(server, CommandType::CreateEntityEx, entity.Release()).type, EntityCreatedType);

    const SceneInfo saved = DecodeSceneInfo(Execute(server, CommandType::SaveSceneToFile, Strings({""})));
    EXPECT_EQ(saved.path, "res://scenes/Hooks.scene") << "a hook that errors does not fail the save";
    EXPECT_EQ(saved.savedRevision, saved.revision) << "the scene is saved";
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"saved res://scenes/Hooks.scene in res://scenes/Hooks.scene"}));
    EXPECT_TRUE(fs::exists(Assets() / "scenes" / "Hooks.scene"));
}

TEST_F(EditorExtensionHooksTest, ASaveThatFailsCallsNoHook)
{
    WriteExtension("a.lua", HookExtension("Saved", "\"scene.read\"",
                                          "    OnSceneSaved = function(info) Editor.Log.Info('saved') end,"));
    Start();
    // A path that is not a scene path: the save is refused before anything is written
    EXPECT_NE(Execute(server, CommandType::SaveSceneToFile, Strings({"not-a-path"})).type, SceneInfoType);
    EXPECT_TRUE(Messages(server).empty());
}

// ==================== OnBeforePlay ====================

TEST_F(EditorExtensionHooksTest, OnBeforePlayWarnsAndNeverBlocksThePlaySnapshot)
{
    WriteExtension("a.lua", HookExtension("Checker", "\"scene.read\"",
                                          "    OnBeforePlay = function(info)\n"
                                          "      return { warnings = { 'no camera in ' .. info.scene, 'no light' } }\n"
                                          "    end,"));
    WriteExtension("b.lua", HookExtension("Broken", "\"scene.read\"",
                                          "    OnBeforePlay = function(info) error('boom') end,"));
    Start();

    const Frame snapshot = Execute(server, CommandType::WritePlaySnapshot, Strings({""}));
    ASSERT_EQ(snapshot.type, PlaySnapshotType) << "warnings and a failing hook do not block play: " << snapshot.Text();
    BufferReader r(snapshot.payload);
    const std::string file = r.ReadString();
    EXPECT_TRUE(fs::exists(fs::path(std::u8string(file.begin(), file.end())))) << "the snapshot was written";

    // The warnings reach every role (an editor pressed Play); the open scene's path says which scene
    const std::vector<json> warnings = EventsOfKind(server, "extensionOutput", EventView::PublicOnly);
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0].at("message"), "no camera in res://scenes/Hooks.scene");
    EXPECT_EQ(warnings[1].at("message"), "no light");
    EXPECT_EQ(warnings[0].at("level"), "warn");
    EXPECT_EQ(warnings[0].at("extension"), "Checker");
}

TEST_F(EditorExtensionHooksTest, OnBeforePlayIsGivenTheScenePlayedWhenOneIsNamed)
{
    WriteExtension("a.lua", HookExtension("Checker", "\"scene.read\"",
                                          "    OnBeforePlay = function(info) return { message = 'playing ' .. info.scene } end,"));
    Start();
    ASSERT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Hooks.scene"})).type, PlaySnapshotType);
    const std::vector<json> warnings = EventsOfKind(server, "extensionOutput", EventView::PublicOnly);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_EQ(warnings[0].at("message"), "playing res://scenes/Hooks.scene");
}

// ==================== OnAssetsChanged ====================

TEST_F(EditorExtensionHooksTest, OnAssetsChangedRunsAfterARescanAndAClientsWrite)
{
    WriteExtension("a.lua", HookExtension("Assets", "\"assets.read\"",
                                          "    OnAssetsChanged = function(added, removed, modified)\n"
                                          "      Editor.Log.Info('added=' .. table.concat(added, ',') .. ' removed=' .. table.concat(removed, ',') "
                                          ".. ' modified=' .. table.concat(modified, ','))\n"
                                          "    end,"));
    Start();

    WriteAsset("one.mat", "{}");
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    ASSERT_EQ(Messages(server).size(), 1u);
    EXPECT_EQ(Messages(server)[0], "added=res://one.mat removed= modified=");

    // A client's own write is a change of the file as well
    ASSERT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://one.mat", "{\"a\":1}"})).type, OkType);
    ASSERT_EQ(Messages(server).size(), 2u);
    EXPECT_EQ(Messages(server)[1], "added= removed= modified=res://one.mat");

    // Nothing changed: no event, and no hook
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    EXPECT_EQ(Messages(server).size(), 2u);

    std::error_code error;
    fs::remove(Assets() / "one.mat", error);
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    ASSERT_EQ(Messages(server).size(), 3u);
    EXPECT_EQ(Messages(server)[2], "added= removed=res://one.mat modified=");
}

TEST_F(EditorExtensionHooksTest, OnAssetsChangedRunsFromTheWatchersPoll)
{
    WriteExtension("a.lua", HookExtension("Assets", "\"assets.read\"",
                                          "    OnAssetsChanged = function(added)\n"
                                          "      Editor.Log.Info('polled ' .. table.concat(added, ','))\n"
                                          "    end,"));
    Start();
    server.SetClientConnected(true);
    WriteAsset("polled.mat", "{}");
    EXPECT_TRUE(server.PollAssetsIfDue());
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"polled res://polled.mat"}));
}

TEST_F(EditorExtensionHooksTest, AnExtensionsOwnAssetWriteDoesNotCallOnAssetsChanged)
{
    WriteExtension("a.lua", HookExtension("Writer", "\"assets.read\", \"assets.write\"",
                                          "    OnAssetsChanged = function(added, removed, modified)\n"
                                          "      Editor.Log.Info('hook ran')\n"
                                          "    end,",
                                          "    { id = 'w.write', path = 'Tools/W', run = function(ctx, p)\n"
                                          "        Editor.Assets.WriteText('res://own.mat', '{}')\n"
                                          "        return { message = 'wrote' }\n"
                                          "      end },"));
    Start();

    // The write happened (the file is there) and the host reloaded it, but the call that made it is still running: no hook
    EXPECT_EQ(ResultMessage(Invoke("w.write")), "wrote");
    EXPECT_EQ(ReadAsset("own.mat"), "{}");
    EXPECT_TRUE(Messages(server).empty()) << "a hook can never start another run of itself";

    // A client's write calls it
    ASSERT_EQ(Execute(server, CommandType::WriteTextAsset, Strings({"res://client.mat", "{}"})).type, OkType);
    EXPECT_EQ(Messages(server), (std::vector<std::string>{"hook ran"}));
}

TEST_F(EditorExtensionHooksTest, HooksReadOnAReadOnlyHostAndStillCannotWrite)
{
    ASSERT_TRUE(server.SetReadOnly(true));
    WriteAsset("existing.txt", "hello from a file");
    WriteExtension("a.lua", HookExtension("Reader", "\"assets.read\", \"assets.write\"",
                                          "    OnAssetsChanged = function(added)\n"
                                          "      Editor.Log.Info('read ' .. Editor.Assets.ReadText('res://existing.txt'))\n"
                                          "      local ok, message = pcall(Editor.Assets.WriteText, 'res://new.txt', 'x')\n"
                                          "      Editor.Log.Info(tostring(ok) .. ' ' .. tostring(message))\n"
                                          "    end,"));
    Start();
    server.SetClientConnected(true);
    WriteAsset("trigger.mat", "{}");
    EXPECT_TRUE(server.PollAssetsIfDue());

    const std::vector<std::string> messages = Messages(server);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], "read hello from a file");
    EXPECT_EQ(messages[1].rfind("false ", 0), 0u) << messages[1];
    EXPECT_NE(messages[1].find("a hook is read-only"), std::string::npos) << messages[1];
    EXPECT_FALSE(fs::exists(Assets() / "new.txt"));
}

// ==================== Editor.Assets ====================

TEST_F(EditorExtensionHooksTest, EditorAssetsReadsWritesListsAndMakesFolders)
{
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    Start();

    EXPECT_EQ(ResultMessage(Invoke("a.write", json{{"path", "res://notes.txt"}, {"text", "line one\nline two"}})), "written");
    EXPECT_EQ(ReadAsset("notes.txt"), "line one\nline two") << "the file is on disk";
    EXPECT_EQ(ResultMessage(Invoke("a.read", json{{"path", "res://notes.txt"}})), "line one\nline two");

    EXPECT_EQ(ResultMessage(Invoke("a.folder", json{{"path", "res://made/deeper"}})), "made");
    EXPECT_TRUE(fs::is_directory(Assets() / "made" / "deeper"));
    // List: this folder only (its folders and the indexed assets in it)
    WriteAsset("listed.mat", "{}");
    ASSERT_EQ(Execute(server, CommandType::RescanAssets).type, OkType);
    const std::string listed = ResultMessage(Invoke("a.list", json{{"path", "res://"}}));
    EXPECT_NE(listed.find("res://made"), std::string::npos) << listed;
    EXPECT_NE(listed.find("res://listed.mat"), std::string::npos) << listed;
    EXPECT_EQ(listed.find("res://made/deeper"), std::string::npos) << "not recursive: " << listed;
    // An empty path is the assets folder
    EXPECT_EQ(ResultMessage(Invoke("a.list")), listed);
    EXPECT_EQ(ResultMessage(Invoke("a.list", json{{"path", "res://made"}})), "res://made/deeper");
}

TEST_F(EditorExtensionHooksTest, EditorAssetsRefusesAnythingButAPathInsideAssets)
{
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    Start();

    const std::vector<std::string> paths = {"res://../escape.txt", "res://a/../../escape.txt", "res://..",
                                            "C:/escape.txt",       "/escape.txt",              "escape.txt",
                                            "res://a\\..\\b.txt",  "res://con.txt",            "res://x:y.txt",
                                            "file:///escape.txt"};
    const std::string host = Root().generic_string();
    for (const std::string &path : paths)
    {
        for (const char *item : {"t.write", "t.read", "t.folder", "t.list"})
        {
            const std::string message = ResultMessage(Invoke(item, json{{"path", path}, {"text", "x"}}));
            EXPECT_EQ(message.rfind("error: ", 0), 0u) << item << " " << path << ": " << message;
            EXPECT_EQ(message.find(host), std::string::npos) << "no host path: " << message;
        }
    }
    EXPECT_FALSE(fs::exists(Root() / "escape.txt"));
    EXPECT_FALSE(fs::exists(Root().parent_path() / "escape.txt"));
    EXPECT_FALSE(fs::exists(Assets() / "escape.txt"));
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("assets.lua"), ExtensionState::Loaded)
        << "a refusal the script caught is no failure";

    // One the script does not catch fails the call, with the host's reason and nothing written
    const Coded failed = DecodeErrorCoded(Invoke("a.write", json{{"path", "res://../escape.txt"}, {"text", "x"}}));
    EXPECT_EQ(failed.code, "extensionFailed");
    EXPECT_EQ(failed.message.find(host), std::string::npos) << failed.message;
    EXPECT_FALSE(fs::exists(Root() / "escape.txt"));
}

TEST_F(EditorExtensionHooksTest, EditorAssetsCannotReachTheEditorFolderThatHoldsTheExtensions)
{
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    Start();
    const std::string before = ReadFile(Root() / "editor" / "assets.lua");
    ASSERT_NE(before.find("Assets Tools"), std::string::npos);

    // res:// is the assets folder: res://editor/evil.lua is assets/editor/evil.lua, never <project>/editor/evil.lua
    EXPECT_EQ(ResultMessage(Invoke("a.write", json{{"path", "res://editor/evil.lua"}, {"text", "return 1"}})), "written");
    EXPECT_TRUE(fs::exists(Assets() / "editor" / "evil.lua"));
    EXPECT_FALSE(fs::exists(Root() / "editor" / "evil.lua"));
    // And the extension's own file cannot be read or overwritten through a path that leaves assets/
    EXPECT_EQ(ResultMessage(Invoke("t.read", json{{"path", "res://../editor/assets.lua"}})).rfind("error: ", 0), 0u);
    EXPECT_EQ(ResultMessage(Invoke("t.write", json{{"path", "res://../editor/assets.lua"}, {"text", "x"}})).rfind("error: ", 0), 0u);
    EXPECT_EQ(ReadFile(Root() / "editor" / "assets.lua"), before);
}

TEST_F(EditorExtensionHooksTest, EditorAssetsHonoursTheTextRulesAndTheSizeCaps)
{
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    Start();

    // Only the text asset kinds the client's own commands handle
    const std::string binary = ResultMessage(Invoke("t.write", json{{"path", "res://picture.png"}, {"text", "x"}}));
    EXPECT_NE(binary.find("Not a text asset"), std::string::npos) << binary;
    EXPECT_FALSE(fs::exists(Assets() / "picture.png"));
    // A folder that does not exist is not made by a write
    const std::string orphan = ResultMessage(Invoke("t.write", json{{"path", "res://nowhere/x.txt"}, {"text", "x"}}));
    EXPECT_EQ(orphan.rfind("error: ", 0), 0u) << orphan;
    EXPECT_FALSE(fs::exists(Assets() / "nowhere"));
    // The open scene's own file is saved with SaveSceneToFile, as for a client
    const std::string scene = ResultMessage(Invoke("t.write", json{{"path", "res://scenes/Hooks.scene"}, {"text", "{}"}}));
    EXPECT_NE(scene.find("SaveSceneToFile"), std::string::npos) << scene;

    // A write over the cap is refused, whatever the script says
    const std::string tooBig = ResultMessage(Invoke("t.big"));
    EXPECT_NE(tooBig.find("are accepted"), std::string::npos) << tooBig;
    EXPECT_FALSE(fs::exists(Assets() / "big.txt"));
    const Coded uncaught = DecodeErrorCoded(Invoke("a.big"));
    EXPECT_EQ(uncaught.code, "extensionFailed");
    EXPECT_FALSE(fs::exists(Assets() / "big.txt"));

    // A file over the cap is not read
    WriteAsset("huge.txt", std::string(EditorServer::MaxTextAssetBytes + 1, 'y'));
    const std::string unread = ResultMessage(Invoke("t.read", json{{"path", "res://huge.txt"}}));
    EXPECT_NE(unread.find("too large to read as text"), std::string::npos) << unread;
}

TEST_F(EditorExtensionHooksTest, ASymbolicLinkInsideAssetsCannotLeadOutOfIt)
{
    const fs::path outside = Root().parent_path() / "outside-the-project";
    std::error_code error;
    fs::create_directories(outside, error);
    {
        std::ofstream secret(outside / "secret.txt", std::ios::binary);
        secret << "outside the project";
    }
    fs::create_directory_symlink(outside, Assets() / "link", error);
    if (error)
    {
        GTEST_SKIP() << "symbolic links cannot be made here: " << error.message();
    }
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    Start();

    EXPECT_EQ(ResultMessage(Invoke("t.read", json{{"path", "res://link/secret.txt"}})).rfind("error: ", 0), 0u);
    EXPECT_EQ(ResultMessage(Invoke("t.write", json{{"path", "res://link/new.txt"}, {"text", "x"}})).rfind("error: ", 0), 0u);
    EXPECT_EQ(ResultMessage(Invoke("t.folder", json{{"path", "res://link/made"}})).rfind("error: ", 0), 0u);
    EXPECT_EQ(ResultMessage(Invoke("t.list", json{{"path", "res://link"}})).rfind("error: ", 0), 0u);
    EXPECT_FALSE(fs::exists(outside / "new.txt"));
    EXPECT_FALSE(fs::exists(outside / "made"));
    // Listing the folder that holds the link does not walk out of it either
    const std::string listed = ResultMessage(Invoke("a.list", json{{"path", "res://"}}));
    EXPECT_EQ(listed.find("secret.txt"), std::string::npos) << listed;
    std::error_code removeError;
    fs::remove_all(outside, removeError);
}

TEST_F(EditorExtensionHooksTest, EditorAssetsFunctionsAreAbsentWithoutTheirCapability)
{
    const auto item = [](const std::string &id, const std::string &body)
    {
        return "    { id = '" + id + "', path = 'Tools/" + id + "', run = function(ctx, p) return { message = " + body +
               " } end },";
    };
    WriteExtension("none.lua", HookExtension("None", "\"scene.read\"", "", item("none.item", "type(Editor.Assets)")));
    WriteExtension("read.lua", HookExtension("Reads", "\"assets.read\"", "",
                                             item("read.item", "type(Editor.Assets.List) .. ' ' .. type(Editor.Assets.ReadText) .. ' ' "
                                                               ".. type(Editor.Assets.WriteText) .. ' ' .. type(Editor.Assets.CreateFolder)")));
    WriteExtension("write.lua", HookExtension("Writes", "\"assets.write\"", "",
                                              item("write.item", "type(Editor.Assets.List) .. ' ' .. type(Editor.Assets.ReadText) .. ' ' "
                                                                 ".. type(Editor.Assets.WriteText) .. ' ' .. type(Editor.Assets.CreateFolder)")));
    Start();
    EXPECT_EQ(ResultMessage(Invoke("none.item")), "nil");
    EXPECT_EQ(ResultMessage(Invoke("read.item")), "function function nil nil");
    EXPECT_EQ(ResultMessage(Invoke("write.item")), "nil nil function function");
}

TEST_F(EditorExtensionHooksTest, AnAssetWriteIsAuditedWithTheExtensionAndTheInvokersIdentity)
{
    LogCapture log;
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    ASSERT_TRUE(server.SetAccessToken("the-one-access-token-0001"));
    ASSERT_TRUE(server.EnableEditorExtensions(Root()));
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes("the-one-access-token-0001")).type, ServerInfoType);

    EXPECT_EQ(ResultMessage(Invoke("a.write", json{{"path", "res://audited.txt"}, {"text", "x"}})), "written");
    EXPECT_TRUE(log.HasLine("audit: user=", "command=WriteTextAsset"));
    EXPECT_TRUE(log.HasLine("command=WriteTextAsset", "via=Assets_Tools"));
    EXPECT_TRUE(log.HasLine("audit: user=", "extension=Assets Tools@"));
}

// ==================== The role table ====================

namespace
{
    struct TokenUser
    {
        std::string name;
        std::string token;
        TokenRole role;
    };

    const TokenUser EditorUser{"ed", "ed-editor-token-0002", TokenRole::Editor};
    const TokenUser ViewerUser{"vic", "vic-viewer-token-0003", TokenRole::Viewer};

    bool WriteTokens(const fs::path &file)
    {
        std::string text = "[";
        bool first = true;
        for (const TokenUser &user : {EditorUser, ViewerUser})
        {
            const auto hash = HashToken(user.token);
            if (!hash.has_value())
                return false;
            text += (first ? "" : ",\n ") + std::string("{\"user\":\"") + user.name + "\",\"role\":\"" +
                    std::string(TokenRoleName(user.role)) + "\",\"hash\":\"" + FormatTokenHash(*hash) + "\"}";
            first = false;
        }
        text += "]\n";
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << text;
        }
#ifndef _WIN32
        fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
        return true;
    }

    std::vector<uint8_t> HelloAs(const TokenUser &user)
    {
        BufferWriter w;
        w.WriteString("hooks role test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(user.token);
        w.WriteBool(true);
        w.WriteString(user.name);
        return w.Release();
    }
}

TEST_F(EditorExtensionHooksTest, ANestedAssetWriteFollowsTheInvokersRoleAndAViewerNeverInvokes)
{
    if (!EditorTlsServer::IsAvailable())
    {
        GTEST_SKIP() << "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";
    }
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    const fs::path tokens = Root() / "tokens.json";
    ASSERT_TRUE(WriteTokens(tokens));
    const auto set = server.SetTokensFile(tokens);
    ASSERT_TRUE(set) << set.error();
    ASSERT_TRUE(server.EnableEditorExtensions(Root()));

    // A viewer is refused at the invocation, and its nested write would be by the role gate too
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloAs(ViewerUser)).type, ServerInfoType);
    EXPECT_EQ(DecodeErrorCoded(Invoke("a.write", json{{"path", "res://viewer.txt"}, {"text", "x"}})).code, "forbidden");
    const Frame nested = Parse(server.DispatchNestedForTest(static_cast<uint8_t>(CommandType::WriteTextAsset),
                                                            Strings({"res://viewer.txt", "x"})));
    EXPECT_EQ(DecodeErrorCoded(nested).code, "forbidden");
    EXPECT_FALSE(fs::exists(Assets() / "viewer.txt"));

    // An editor may write assets, as it may with its own WriteTextAsset
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloAs(EditorUser)).type, ServerInfoType);
    EXPECT_EQ(ResultMessage(Invoke("a.write", json{{"path", "res://editor-wrote.txt"}, {"text", "ok"}})), "written");
    EXPECT_EQ(ReadAsset("editor-wrote.txt"), "ok");
}

// ==================== The read-only host ====================

TEST_F(EditorExtensionHooksTest, AReadOnlyHostRefusesTheInvocationSoNoAssetIsWritten)
{
    WriteExtension("assets.lua", HookExtension("Assets Tools", "\"assets.read\", \"assets.write\"", "", AssetItems));
    ASSERT_TRUE(server.SetReadOnly(true));
    Start();
    EXPECT_EQ(DecodeErrorCoded(Invoke("a.write", json{{"path", "res://nope.txt"}, {"text", "x"}})).code, "readOnly");
    EXPECT_FALSE(fs::exists(Assets() / "nope.txt"));
    // The nested command would be refused by the read-only gate too
    const Frame nested = Parse(server.DispatchNestedForTest(static_cast<uint8_t>(CommandType::WriteTextAsset),
                                                            Strings({"res://nope.txt", "x"})));
    EXPECT_EQ(DecodeErrorCoded(nested).code, "readOnly");
    EXPECT_FALSE(fs::exists(Assets() / "nope.txt"));
}
