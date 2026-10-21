#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/ExtensionHost.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Inspector sections of Lua editor extensions (E-X5, protocol 1.16.0) on a host with no scene: what
// Editor.Inspector.Register declares is captured when the file loads, validated (a bad shape fails the file), and returned
// by GetEditorUi; button ids are unique across the host; a registration outside the load is refused; and the gates of
// InvokeInspectorAction that come before the component is looked up (the flag, read-only, an unknown button, no scene,
// the classification). What needs a scene (one undo step, the rollback, the component checks, the roles) is in
// project/EditorExtensionInspectorTests.cpp.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t EditorUiType = static_cast<uint8_t>(ResponseType::EditorUi);

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

    std::vector<uint8_t> InspectorPayload(const std::string &componentId, const std::string &actionId,
                                          const uint32_t uiRevision, const json &params = json::object())
    {
        BufferWriter w;
        w.WriteString(componentId);
        w.WriteString(actionId);
        WriteJson(w, params);
        w.WriteU32(uiRevision);
        return w.Release();
    }

    std::vector<uint8_t> HelloWithErrorCodes()
    {
        BufferWriter w;
        w.WriteString("inspector test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString("");
        w.WriteBool(true);
        return w.Release();
    }

    std::string CodeOf(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ErrorCodedType) << frame.Text();
        if (frame.type != ErrorCodedType)
            return {};
        BufferReader r(frame.payload);
        (void)r.ReadString();
        return r.ReadString();
    }

    class TempProject
    {
    public:
        TempProject()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-inspector-" + std::to_string(stamp) + "-" +
                                                 std::to_string(counter++));
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
            std::ofstream file(_root / "editor" / relative, std::ios::binary | std::ios::trunc);
            file << text;
        }

    private:
        fs::path _root;
    };

    // The registry entry of one file
    json EntryOf(const ExtensionHost &host, const std::string &id)
    {
        for (const json &entry : host.DescribeJson().at("extensions"))
        {
            if (entry.at("id") == id)
                return entry;
        }
        return json();
    }

    // A file that registers `registration` (the Lua text of the call's arguments) with the given capabilities
    std::string Registering(const std::string &registration,
                            const std::string &capabilities = "\"scene.read\", \"scene.write\"")
    {
        return "Editor.Inspector.Register(" + registration + ")\n"
               "return Editor.Extension { name = \"Sections\", apiVersion = 1, capabilities = { " + capabilities + " } }\n";
    }

    constexpr const char *ValidRegistration = R"LUA("Light", {
  sections = {
    {
      label = "Light tools",
      fields = { intensity = { type = "number", min = 0, max = 10, tooltip = "How bright" }, on = { type = "bool" } },
      buttons = {
        { id = "light.brighten", label = "Brighten", params = { by = { type = "int", default = 2, min = 1, max = 5 } },
          run = function(ctx, p) return nil end },
        { id = "light.reset", label = "Reset", minRole = "admin", run = function(ctx, p) return nil end },
      },
    },
    { label = "Buttons only", buttons = { { id = "light.ping", label = "Ping", run = function(ctx, p) return nil end } } },
  },
})LUA";

    class ExtensionInspectorTest : public ::testing::Test
    {
    protected:
        void SetUp() override { _watchdog.emplace(std::chrono::seconds(120)); }
        void TearDown() override { _watchdog.reset(); }

        std::optional<Tests::HangWatchdog> _watchdog;
    };
}

// ==================== The declaration ====================

TEST_F(ExtensionInspectorTest, SectionsAreCapturedAtLoadAndReturnedByGetEditorUi)
{
    TempProject project;
    project.Write("a.lua", Registering(ValidRegistration));
    ExtensionHost host(project.Root());
    host.Load();
    ASSERT_EQ(EntryOf(host, "a.lua").at("state"), "loaded") << EntryOf(host, "a.lua").dump();

    const json sections = host.UiJson().at("inspectorSections");
    ASSERT_EQ(sections.size(), 2u) << sections.dump();
    const json &tools = sections[0];
    EXPECT_EQ(tools.at("componentType"), "Light");
    EXPECT_EQ(tools.at("extension"), "a.lua");
    EXPECT_EQ(tools.at("label"), "Light tools");
    // The fields are FieldSchema, sorted by name, built with the same kind rules as a form's
    ASSERT_EQ(tools.at("fields").size(), 2u);
    EXPECT_EQ(tools.at("fields")[0].at("name"), "intensity");
    EXPECT_EQ(tools.at("fields")[0].at("kind"), "Float");
    EXPECT_EQ(tools.at("fields")[0].at("min"), 0);
    EXPECT_EQ(tools.at("fields")[0].at("max"), 10);
    EXPECT_EQ(tools.at("fields")[0].at("tooltip"), "How bright");
    EXPECT_EQ(tools.at("fields")[1].at("name"), "on");
    EXPECT_EQ(tools.at("fields")[1].at("kind"), "Bool");
    // The buttons in the order declared, the editor role by default, and a form for the one with params
    ASSERT_EQ(tools.at("buttons").size(), 2u);
    const json &brighten = tools.at("buttons")[0];
    EXPECT_EQ(brighten.at("id"), "light.brighten");
    EXPECT_EQ(brighten.at("label"), "Brighten");
    EXPECT_EQ(brighten.at("minRole"), "editor");
    EXPECT_EQ(brighten.at("form").at("fields")[0].at("kind"), "Int");
    EXPECT_EQ(brighten.at("form").at("defaults").at("by"), 2);
    EXPECT_FALSE(brighten.contains("run")) << "no function reaches the wire";
    EXPECT_EQ(tools.at("buttons")[1].at("minRole"), "admin");
    EXPECT_FALSE(tools.at("buttons")[1].contains("form"));
    EXPECT_EQ(sections[1].at("label"), "Buttons only");
    EXPECT_TRUE(sections[1].at("fields").empty());

    // FindInspectorAction knows each button with its section's type
    const auto found = host.FindInspectorAction("light.reset");
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->inspectorAction);
    EXPECT_EQ(found->componentType, "Light");
    EXPECT_EQ(found->title, "Reset");
    EXPECT_EQ(found->minRole, "admin");
    EXPECT_FALSE(host.FindInspectorAction("no.such").has_value());
    EXPECT_FALSE(host.FindMenuItem("light.reset").has_value()) << "a button is not a menu item";
}

TEST_F(ExtensionInspectorTest, ABadSectionShapeFailsTheFileWithAReason)
{
    const std::vector<std::pair<std::string, std::string>> cases = {
        // A section reads its component: the design grants it with scene.read
        {Registering(ValidRegistration, "\"scene.write\""), "need the capability scene.read"},
        {Registering("\"Bad Type!\", { sections = { { label = \"L\", buttons = { { id = \"b\", label = \"B\", "
                     "run = function() end } } } } }"),
         "must be a type name"},
        {Registering("5, { sections = {} }"), "expects a component type name"},
        {Registering("\"Light\", \"not a table\""), "expects a table such as { sections"},
        {Registering("\"Light\", { sections = {} }"), "'sections' must be a list of at least one section"},
        {Registering("\"Light\", { sections = { { label = \"L\" } } }"), "a section needs 'fields' or 'buttons'"},
        {Registering("\"Light\", { sections = { { label = \"L\", colour = 1, buttons = {} } } }"), "unknown key 'colour'"},
        {Registering("\"Light\", { sections = { { label = \"L\", fields = { a = { type = \"number\", default = 1 } } } } }"),
         "has a 'default'"},
        {Registering("\"Light\", { sections = { { label = \"L\", buttons = { { id = \"b\", label = \"B\" } } } } }"),
         "'run' must be a function"},
        {Registering("\"Light\", { sections = { { label = \"L\", buttons = { { id = \"b\", label = \"B\", minRole = "
                     "\"viewer\", run = function() end } } } } }"),
         "'minRole' must be"},
        {Registering("\"Light\", { sections = { { label = \"L\", buttons = { { id = \"b\", label = \"B\", run = function() "
                     "end }, { id = \"b\", label = \"C\", run = function() end } } } } }"),
         "used by two inspector buttons"},
        {Registering("\"Light\", { sections = { { buttons = { { id = \"b\", label = \"B\", run = function() end } } } } }"),
         "'label' is required"},
    };
    for (const auto &[source, expected] : cases)
    {
        TempProject project;
        project.Write("a.lua", source);
        project.Write("z.lua", Registering(ValidRegistration));
        ExtensionHost host(project.Root());
        host.Load();
        const json entry = EntryOf(host, "a.lua");
        EXPECT_EQ(entry.at("state"), "failed") << source;
        EXPECT_NE(entry.value("error", std::string()).find(expected), std::string::npos)
            << "expected \"" << expected << "\" in: " << entry.value("error", std::string());
        // A broken file never stops the others
        EXPECT_EQ(EntryOf(host, "z.lua").at("state"), "loaded") << source;
        EXPECT_EQ(host.UiJson().at("inspectorSections").size(), 2u) << source;
    }
}

TEST_F(ExtensionInspectorTest, ButtonIdsAreUniqueAcrossTheHostApartFromMenuItemIds)
{
    TempProject project;
    project.Write("a.lua", Registering(ValidRegistration));
    // The same button id in a later file: that file fails, the first keeps it
    project.Write("b.lua", Registering("\"Light\", { sections = { { label = \"L\", buttons = { { id = \"light.ping\", "
                                       "label = \"B\", run = function() end } } } } }"));
    // A menu item may have a button's id: the two are named by different commands
    project.Write("c.lua", "return Editor.Extension { name = \"Menu\", apiVersion = 1, menu = { { id = \"light.ping\", "
                           "path = \"Tools/Ping\", run = function() end } } }\n");
    ExtensionHost host(project.Root());
    host.Load();
    EXPECT_EQ(EntryOf(host, "a.lua").at("state"), "loaded");
    const json clash = EntryOf(host, "b.lua");
    EXPECT_EQ(clash.at("state"), "failed");
    EXPECT_NE(clash.value("error", std::string()).find("The inspector button id \"light.ping\" is already used by the "
                                                        "extension a.lua"),
              std::string::npos)
        << clash.dump();
    EXPECT_EQ(EntryOf(host, "c.lua").at("state"), "loaded") << EntryOf(host, "c.lua").dump();
    EXPECT_EQ(host.FindInspectorAction("light.ping")->extensionId, "a.lua");
    EXPECT_EQ(host.FindMenuItem("light.ping")->extensionId, "c.lua");
}

TEST_F(ExtensionInspectorTest, ARegistrationOutsideTheLoadIsRefused)
{
    TempProject project;
    project.Write("a.lua", "return Editor.Extension { name = \"Late\", apiVersion = 1, capabilities = { \"scene.read\" }, "
                           "menu = { { id = \"late.item\", path = \"Tools/Late\", run = function(ctx, p)\n"
                           "  Editor.Inspector.Register(\"Light\", { sections = { { label = \"L\", buttons = {} } } })\n"
                           "end } } }\n");
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    BufferWriter w;
    w.WriteString("late.item");
    WriteJson(w, json::array());
    WriteJson(w, json::object());
    w.WriteU32(server.GetEditorExtensions()->UiRevision());
    const Frame frame = Execute(server, CommandType::InvokeMenuItem, w.Release());
    ASSERT_EQ(frame.type, ErrorCodedType) << frame.Text();
    BufferReader r(frame.payload);
    const std::string message = r.ReadString();
    EXPECT_EQ(r.ReadString(), "extensionFailed");
    EXPECT_NE(message.find("can be called only while the file loads"), std::string::npos) << message;
    EXPECT_TRUE(server.GetEditorExtensions()->UiJson().at("inspectorSections").empty());
    (void)server.ShutdownEditorExtensions(std::chrono::milliseconds(2000));
}

// ==================== The gates without a scene ====================

TEST_F(ExtensionInspectorTest, TheTableClassifiesItAsAnEditOnlyWriteThatAScriptMayNotSend)
{
    const auto id = static_cast<uint8_t>(CommandType::InvokeInspectorAction);
    EXPECT_EQ(EditorServer::ClassifyCommand(id), EditorServer::CommandClass::Write);
    EXPECT_FALSE(EditorServer::IsReadCommand(id));
    EXPECT_TRUE(EditorServer::IsEditOnlyCommand(id)) << "a play host refuses it (project/EditorExtensionPlayTests.cpp)";
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id));
    const std::optional<bool> nested = EditorServer::NestedCommandPolicy(id);
    ASSERT_TRUE(nested.has_value()) << "decided for the nested dispatch";
    EXPECT_FALSE(*nested) << "no recursion";
    EXPECT_EQ(EditorServer::CommandName(id), "InvokeInspectorAction");
}

TEST_F(ExtensionInspectorTest, AHostWithoutTheFlagAnswersExtensionsDisabled)
{
    EditorServer server;
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    EXPECT_EQ(CodeOf(Execute(server, CommandType::InvokeInspectorAction,
                             InspectorPayload("01234567-89ab-cdef-0123-456789abcdef", "light.ping", 0))),
              "extensionsDisabled");
}

TEST_F(ExtensionInspectorTest, AReadOnlyHostRefusesItAndGetEditorUiStillAnswers)
{
    TempProject project;
    project.Write("a.lua", Registering(ValidRegistration));
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    const uint32_t revision = server.GetEditorExtensions()->UiRevision();

    const Frame plain = Execute(server, CommandType::InvokeInspectorAction,
                                InspectorPayload("01234567-89ab-cdef-0123-456789abcdef", "light.ping", revision));
    EXPECT_EQ(plain.type, ErrorType);
    EXPECT_EQ(plain.Text(), "read-only host: InvokeInspectorAction is not allowed");
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    EXPECT_EQ(CodeOf(Execute(server, CommandType::InvokeInspectorAction,
                             InspectorPayload("01234567-89ab-cdef-0123-456789abcdef", "light.ping", revision))),
              "readOnly");

    const Frame ui = Execute(server, CommandType::GetEditorUi);
    ASSERT_EQ(ui.type, EditorUiType) << ui.Text();
    BufferReader r(ui.payload);
    EXPECT_EQ(json::parse(r.ReadString()).at("inspectorSections").size(), 2u);
    (void)server.ShutdownEditorExtensions(std::chrono::milliseconds(2000));
}

TEST_F(ExtensionInspectorTest, AnUnknownButtonAndAMissingSceneAreErrorsBeforeAnyLuaRuns)
{
    TempProject project;
    project.Write("a.lua", Registering(ValidRegistration));
    EditorServer server;
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    const uint32_t revision = server.GetEditorExtensions()->UiRevision();

    const Frame unknown = Execute(server, CommandType::InvokeInspectorAction,
                                  InspectorPayload("01234567-89ab-cdef-0123-456789abcdef", "no.such", revision));
    EXPECT_EQ(unknown.type, ErrorType);
    EXPECT_EQ(unknown.Text(), "Unknown inspector button: no.such");

    const Frame noScene = Execute(server, CommandType::InvokeInspectorAction,
                                  InspectorPayload("01234567-89ab-cdef-0123-456789abcdef", "light.ping", revision));
    EXPECT_EQ(noScene.type, ErrorType);
    EXPECT_EQ(noScene.Text(), "No scene loaded");

    EXPECT_EQ(CodeOf(Execute(server, CommandType::InvokeInspectorAction,
                             InspectorPayload("01234567-89ab-cdef-0123-456789abcdef", "light.ping", revision + 1))),
              "staleUiRevision");
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);
    (void)server.ShutdownEditorExtensions(std::chrono::milliseconds(2000));
}
