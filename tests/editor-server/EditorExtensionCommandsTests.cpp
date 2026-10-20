#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/ExtensionHost.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>
#include <engine/scripting/LuaSandbox.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// The Lua editor extension commands (protocol 1.14.0) through EditorServer::ExecuteCommand: what a host without
// --editor-extensions answers, what one with it lists, ReloadEditorExtensions and its event, the read-only gate and the
// roles, the reserved ids, and the proof that the two reads run no Lua. ExtensionHostTests.cpp covers the host itself.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t EditorExtensionsType = static_cast<uint8_t>(ResponseType::EditorExtensions);
    constexpr uint8_t EditorUiType = static_cast<uint8_t>(ResponseType::EditorUi);

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return std::string(payload.begin(), payload.end()); }
    };

    Frame Execute(EditorServer &server, uint8_t id, const std::vector<uint8_t> &payload = {})
    {
        const std::vector<uint8_t> frame = server.ExecuteCommand(id, payload);
        BufferReader r(frame);
        Frame out;
        out.type = r.ReadU8();
        const std::span<const uint8_t> body = r.ReadBytes(r.ReadU32());
        out.payload.assign(body.begin(), body.end());
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the response payload";
        return out;
    }

    Frame Execute(EditorServer &server, CommandType type, const std::vector<uint8_t> &payload = {})
    {
        return Execute(server, static_cast<uint8_t>(type), payload);
    }

    // The JSON a response of one json field carries (EditorExtensions' info, EditorUi's ui)
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

    std::vector<uint8_t> HelloWithErrorCodes()
    {
        BufferWriter w;
        w.WriteString("extension test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString("");
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

    class TempProject
    {
    public:
        TempProject()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-cmd-" + std::to_string(stamp) + "-" + std::to_string(counter++));
            fs::create_directories(_root / "editor");
        }

        ~TempProject()
        {
            std::error_code error;
            fs::remove_all(_root, error);
        }

        TempProject(const TempProject &) = delete;
        TempProject &operator=(const TempProject &) = delete;

        [[nodiscard]] const fs::path &Root() const { return _root; }

        void Write(const std::string &relative, const std::string &text) const
        {
            const fs::path path = _root / "editor" / relative;
            fs::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << text;
        }

    private:
        fs::path _root;
    };

    std::string ExtensionText(const std::string &name, const std::string &itemId)
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = { \"scene.read\" },\n"
               "  menu = { { id = \"" + itemId + "\", path = \"Tools/" + name + "\", run = function() end } },\n"
               "}\n";
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

    std::atomic<long> g_instructions{0};

    void CountInstructions(lua_State *, lua_Debug *)
    {
        ++g_instructions;
    }
}

// ==================== A host without --editor-extensions ====================

TEST(EditorExtensionCommandsTest, AHostWithoutTheFlagAnswersDisabledNotAnError)
{
    TempProject project;
    project.Write("a.lua", ExtensionText("A", "a.item")); // a file is there, but nothing asked for it to be read
    EditorServer server;
    const int statesBefore = Scripting::LuaSandbox::LiveStates();

    const json list = DecodeJson(Execute(server, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_FALSE(list.at("enabled").get<bool>());
    EXPECT_EQ(list.at("uiRevision"), 0);
    EXPECT_TRUE(list.at("extensions").empty());

    const json ui = DecodeJson(Execute(server, CommandType::GetEditorUi), EditorUiType);
    EXPECT_EQ(ui.at("uiRevision"), 0);
    EXPECT_TRUE(ui.at("menuItems").empty());
    EXPECT_TRUE(ui.at("inspectorSections").empty());

    EXPECT_FALSE(server.EditorExtensionsEnabled());
    EXPECT_EQ(server.GetEditorExtensions(), nullptr);
    EXPECT_EQ(Scripting::LuaSandbox::LiveStates(), statesBefore) << "no state was made: nothing loads without the flag";
    EXPECT_EQ(server.ShutdownEditorExtensions(std::chrono::milliseconds(10)), 0);
}

TEST(EditorExtensionCommandsTest, ReloadOnAHostWithoutTheFlagIsRefusedWithTheExtensionsDisabledCode)
{
    EditorServer server;
    // A client that did not ask for codes gets an Error whose message says why
    const Frame plain = Execute(server, CommandType::ReloadEditorExtensions);
    EXPECT_EQ(plain.type, ErrorType);
    EXPECT_NE(plain.Text().find("--editor-extensions"), std::string::npos) << plain.Text();

    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    const Coded coded = DecodeErrorCoded(Execute(server, CommandType::ReloadEditorExtensions));
    EXPECT_EQ(coded.code, "extensionsDisabled");
    EXPECT_EQ(coded.code, std::string{ErrorCodeExtensionsDisabled});
    EXPECT_NE(coded.message.find("--editor-extensions"), std::string::npos) << coded.message;
}

// ==================== A host with it ====================

TEST(EditorExtensionCommandsTest, AHostWithTheFlagListsWhatLoadedAndWhatFailed)
{
    TempProject project;
    project.Write("a_ok.lua", ExtensionText("Good", "good.item"));
    project.Write("b_broken.lua", "error('boom')");
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    EXPECT_TRUE(server.EditorExtensionsEnabled());
    EXPECT_TRUE(EventsOfKind(server, "extensionsChanged").empty()) << "loading at start is not a change";

    const json list = DecodeJson(Execute(server, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_TRUE(list.at("enabled").get<bool>());
    EXPECT_EQ(list.at("uiRevision"), 1);
    ASSERT_EQ(list.at("extensions").size(), 2u);
    const json &good = list.at("extensions")[0];
    EXPECT_EQ(good.at("id"), "a_ok.lua");
    EXPECT_EQ(good.at("name"), "Good");
    EXPECT_EQ(good.at("state"), "loaded");
    EXPECT_EQ(good.at("sha256").get<std::string>().size(), 64u);
    EXPECT_EQ(good.at("capabilities"), (json::array({"scene.read"})));
    EXPECT_EQ(good.at("itemCount"), 1);
    const json &broken = list.at("extensions")[1];
    EXPECT_EQ(broken.at("state"), "failed");
    EXPECT_NE(broken.at("error").get<std::string>().find("boom"), std::string::npos);

    const json ui = DecodeJson(Execute(server, CommandType::GetEditorUi), EditorUiType);
    EXPECT_EQ(ui.at("uiRevision"), 1);
    ASSERT_EQ(ui.at("menuItems").size(), 1u);
    EXPECT_EQ(ui.at("menuItems")[0].at("id"), "good.item");
    EXPECT_EQ(ui.at("menuItems")[0].at("extension"), "a_ok.lua");
    EXPECT_EQ(ui.at("menuItems")[0].at("minRole"), "editor");
}

TEST(EditorExtensionCommandsTest, ReloadPicksUpTheFilesAgainAndSaysSo)
{
    TempProject project;
    project.Write("a.lua", ExtensionText("Before", "a.item"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    project.Write("a.lua", ExtensionText("After", "a.item"));
    project.Write("b.lua", ExtensionText("Added", "b.item"));
    // Not until asked
    EXPECT_EQ(DecodeJson(Execute(server, CommandType::ListEditorExtensions), EditorExtensionsType)
                  .at("extensions").size(), 1u);

    const json reloaded = DecodeJson(Execute(server, CommandType::ReloadEditorExtensions), EditorExtensionsType);
    EXPECT_TRUE(reloaded.at("enabled").get<bool>());
    EXPECT_EQ(reloaded.at("uiRevision"), 2);
    ASSERT_EQ(reloaded.at("extensions").size(), 2u);
    EXPECT_EQ(reloaded.at("extensions")[0].at("name"), "After");
    EXPECT_EQ(reloaded.at("extensions")[1].at("name"), "Added");

    const json ui = DecodeJson(Execute(server, CommandType::GetEditorUi), EditorUiType);
    EXPECT_EQ(ui.at("uiRevision"), 2);
    EXPECT_EQ(ui.at("menuItems").size(), 2u);

    const std::vector<json> changed = EventsOfKind(server, "extensionsChanged");
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].at("uiRevision"), 2);
    EXPECT_GT(changed[0].at("seq").get<uint32_t>(), 0u);

    ASSERT_EQ(Execute(server, CommandType::ReloadEditorExtensions).type, EditorExtensionsType);
    EXPECT_EQ(EventsOfKind(server, "extensionsChanged").size(), 2u);
    EXPECT_EQ(EventsOfKind(server, "extensionsChanged")[1].at("uiRevision"), 3);
}

TEST(EditorExtensionCommandsTest, AnExtensionThatBecomesFaultedMovesTheRevisionAndSaysSo)
{
    TempProject project;
    project.Write("a.lua", ExtensionText("A", "a.item"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    ExtensionHost *host = server.GetEditorExtensions();
    ASSERT_NE(host, nullptr);

    const auto start = std::chrono::steady_clock::now();
    (void)host->NoteFailure("a.lua", start);
    (void)host->NoteFailure("a.lua", start + std::chrono::seconds(1));
    EXPECT_TRUE(EventsOfKind(server, "extensionsChanged").empty());
    EXPECT_EQ(host->NoteFailure("a.lua", start + std::chrono::seconds(2)), ExtensionState::Faulted);

    const std::vector<json> changed = EventsOfKind(server, "extensionsChanged");
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed[0].at("uiRevision"), 2);
    const json list = DecodeJson(Execute(server, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_EQ(list.at("uiRevision"), 2);
    EXPECT_EQ(list.at("extensions")[0].at("state"), "faulted");
    // Still listed, so the client can show it disabled
    EXPECT_EQ(DecodeJson(Execute(server, CommandType::GetEditorUi), EditorUiType).at("menuItems").size(), 1u);
}

// ==================== The two reads run no Lua ====================

TEST(EditorExtensionCommandsTest, ListAndGetEditorUiRunNoLua)
{
    TempProject project;
    project.Write("a.lua", ExtensionText("A", "a.item"));
    project.Write("b.lua", ExtensionText("B", "b.item"));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    ExtensionHost *host = server.GetEditorExtensions();
    ASSERT_NE(host, nullptr);
    ASSERT_EQ(host->LoadedCount(), 2u);

    // A counting hook on every state: the control first (a chunk run in each is counted)
    int states = 0;
    g_instructions = 0;
    host->ForEachStateForTest([&states](const std::string &, lua_State *state)
    {
        lua_sethook(state, &CountInstructions, LUA_MASKCOUNT, 1);
        ++states;
        EXPECT_EQ(luaL_dostring(state, "local n = 0 for i = 1, 100 do n = n + i end"), LUA_OK);
    });
    EXPECT_EQ(states, 2);
    EXPECT_GT(g_instructions.load(), 200) << "the counter sees Lua run";

    g_instructions = 0;
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ(Execute(server, CommandType::ListEditorExtensions).type, EditorExtensionsType);
        EXPECT_EQ(Execute(server, CommandType::GetEditorUi).type, EditorUiType);
    }
    EXPECT_EQ(g_instructions.load(), 0) << "ListEditorExtensions and GetEditorUi ran Lua";
}

// ==================== Read-only, roles, play, reserved ids ====================

TEST(EditorExtensionCommandsTest, AReadOnlyHostListsExtensionsButCannotReloadThem)
{
    TempProject project;
    project.Write("a.lua", ExtensionText("A", "a.item"));
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    const json list = DecodeJson(Execute(server, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_TRUE(list.at("enabled").get<bool>());
    EXPECT_EQ(list.at("extensions").size(), 1u);
    EXPECT_EQ(DecodeJson(Execute(server, CommandType::GetEditorUi), EditorUiType).at("menuItems").size(), 1u);

    const Frame refused = Execute(server, CommandType::ReloadEditorExtensions);
    EXPECT_EQ(refused.type, ErrorType);
    EXPECT_EQ(refused.Text(), "read-only host: ReloadEditorExtensions is not allowed");
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    EXPECT_EQ(DecodeErrorCoded(Execute(server, CommandType::ReloadEditorExtensions)).code, "readOnly");
    EXPECT_TRUE(EventsOfKind(server, "extensionsChanged").empty()) << "a refused reload changed nothing";
}

TEST(EditorExtensionCommandsTest, AViewerAndAnEditorMayListOnlyAnAdminMayReload)
{
    const auto id = [](const CommandType type) { return static_cast<uint8_t>(type); };
    for (const TokenRole role : {TokenRole::Viewer, TokenRole::Editor, TokenRole::Admin})
    {
        EXPECT_TRUE(EditorServer::RoleAllows(role, id(CommandType::ListEditorExtensions))) << TokenRoleName(role);
        EXPECT_TRUE(EditorServer::RoleAllows(role, id(CommandType::GetEditorUi))) << TokenRoleName(role);
    }
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id(CommandType::ReloadEditorExtensions)));
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Editor, id(CommandType::ReloadEditorExtensions)));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id(CommandType::ReloadEditorExtensions)));

    EXPECT_EQ(EditorServer::ClassifyCommand(id(CommandType::ListEditorExtensions)), EditorServer::CommandClass::Read);
    EXPECT_EQ(EditorServer::ClassifyCommand(id(CommandType::GetEditorUi)), EditorServer::CommandClass::Read);
    EXPECT_EQ(EditorServer::ClassifyCommand(id(CommandType::ReloadEditorExtensions)), EditorServer::CommandClass::Admin);
    EXPECT_EQ(EditorServer::RoleDeniedMessage(TokenRole::Editor, id(CommandType::ReloadEditorExtensions)),
              "forbidden: role editor is not allowed to run ReloadEditorExtensions");
    EXPECT_EQ(EditorServer::CommandName(id(CommandType::GetEditorUi)), "GetEditorUi");
}

TEST(EditorExtensionCommandsTest, APlayHostRefusesTheReloadAndAnswersTheLists)
{
    EXPECT_TRUE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::ReloadEditorExtensions)));
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::ListEditorExtensions)));
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::GetEditorUi)));
}

TEST(EditorExtensionCommandsTest, TheIdsOfTheLaterInvokeCommandsAreReservedAndRefused)
{
    for (const uint8_t reserved : {uint8_t{0x82}, uint8_t{0x83}})
    {
        EXPECT_FALSE(EditorServer::ClassifyCommand(reserved).has_value()) << int{reserved};
        EXPECT_FALSE(EditorServer::IsReadCommand(reserved)) << int{reserved};
        EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Editor, reserved)) << int{reserved};
        EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, reserved)) << int{reserved};

        EditorServer server;
        const Frame unknown = Execute(server, reserved);
        EXPECT_EQ(unknown.type, ErrorType);
        EXPECT_EQ(unknown.Text(), "Unknown command") << "nothing implements it yet";

        EditorServer readOnly;
        ASSERT_TRUE(readOnly.SetReadOnly(true));
        EXPECT_EQ(Execute(readOnly, reserved).Text(), EditorServer::ReadOnlyMessage(reserved));
    }
}

TEST(EditorExtensionCommandsTest, TheSpecHasTheEventKeyTheShapesAndTheResponsesTheCommandsAnswerWith)
{
    std::ifstream file(N2_PROTOCOL_JSON);
    ASSERT_TRUE(file.is_open()) << N2_PROTOCOL_JSON;
    const json spec = json::parse(file);
    for (const auto &[code, meaning] : spec.at("errorCodes").items())
        EXPECT_FALSE(meaning.get<std::string>().empty()) << code;
    EXPECT_EQ(spec.at("version"), "1.14.0");

    // The event key and the JSON shapes the extension commands answer with
    EXPECT_TRUE(spec.at("jsonTypes").at("EditorEvent").contains("uiRevision?"));
    for (const char *shape : {"ExtensionInfo", "EditorExtensionsInfo", "MenuForm", "MenuItemInfo", "EditorUiInfo"})
        EXPECT_TRUE(spec.at("jsonTypes").contains(shape)) << shape;
    EXPECT_EQ(spec.at("responses").at("EditorExtensions"), "0x1D");
    EXPECT_EQ(spec.at("responses").at("EditorUi"), "0x1E");
}

TEST(EditorExtensionCommandsTest, TheProtocolVersionAndCapabilityAreThose1_14_0Adds)
{
    EXPECT_EQ(std::string{ProtocolVersion}, "1.14.0");
    const std::vector<std::string> capabilities = EditorServer::Capabilities();
    ASSERT_EQ(capabilities.size(), 1u);
    EXPECT_EQ(capabilities[0], "editorExtensions");
    // The three codes a client branches on (two of them are answered by later commands)
    EXPECT_EQ(std::string{ErrorCodeExtensionsDisabled}, "extensionsDisabled");
    EXPECT_EQ(std::string{ErrorCodeExtensionFailed}, "extensionFailed");
    EXPECT_EQ(std::string{ErrorCodeExtensionFaulted}, "extensionFaulted");
    EXPECT_EQ(static_cast<int>(CommandType::ListEditorExtensions), 0x80);
    EXPECT_EQ(static_cast<int>(CommandType::GetEditorUi), 0x81);
    EXPECT_EQ(static_cast<int>(CommandType::ReloadEditorExtensions), 0x84);
}
