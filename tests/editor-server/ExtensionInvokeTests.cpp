#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// InvokeMenuItem (protocol 1.15.0) and the nested dispatch of editor extensions, through EditorServer::ExecuteCommand on a
// host with no scene: the gates (flag, stale uiRevision, open edit group, read-only, roles, minRole, faulted), the form
// and selection checks, the policy table of what a script may send (every command of the spec decided, every denied id
// refused), the capabilities (a function of an ungranted one is absent), Editor.Log and its events, the nested cap, a
// budget trip, a stalled worker that is abandoned, and the path-stripped errors. What needs a scene (one undo step, the
// rollback, the golden Snap-to-grid example) is in project/EditorExtensionInvokeTests.cpp.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t EditorExtensionsType = static_cast<uint8_t>(ResponseType::EditorExtensions);
    constexpr uint8_t ExtensionResultType = static_cast<uint8_t>(ResponseType::ExtensionResult);
    constexpr uint8_t ComponentTypesType = static_cast<uint8_t>(ResponseType::ComponentTypes);

    struct Frame
    {
        uint8_t type = 0xEE;
        std::vector<uint8_t> payload;

        [[nodiscard]] std::string Text() const { return std::string(payload.begin(), payload.end()); }
    };

    Frame Parse(const std::vector<uint8_t> &frame)
    {
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
        return Parse(server.ExecuteCommand(static_cast<uint8_t>(type), payload));
    }

    std::vector<uint8_t> InvokePayload(const std::string &itemId, const json &selection, const json &params,
                                       const uint32_t uiRevision)
    {
        BufferWriter w;
        w.WriteString(itemId);
        WriteJson(w, selection);
        WriteJson(w, params);
        w.WriteU32(uiRevision);
        return w.Release();
    }

    // InvokeMenuItem with the host's current uiRevision
    Frame Invoke(EditorServer &server, const std::string &itemId, const json &params = json::object(),
                 const json &selection = json::array())
    {
        const uint32_t revision = server.GetEditorExtensions() != nullptr ? server.GetEditorExtensions()->UiRevision() : 0;
        return Execute(server, CommandType::InvokeMenuItem, InvokePayload(itemId, selection, params, revision));
    }

    struct Result
    {
        std::string label;
        uint32_t revision = 0;
        bool canUndo = false;
        bool canRedo = false;
        uint32_t savedRevision = 0;
        json result;
    };

    Result DecodeResult(const Frame &frame)
    {
        EXPECT_EQ(frame.type, ExtensionResultType) << frame.Text();
        Result decoded;
        if (frame.type != ExtensionResultType)
            return decoded;
        BufferReader r(frame.payload);
        decoded.label = r.ReadString();
        decoded.revision = r.ReadU32();
        decoded.canUndo = r.ReadBool();
        decoded.canRedo = r.ReadBool();
        decoded.savedRevision = r.ReadU32();
        decoded.result = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the result";
        return decoded;
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

    std::vector<uint8_t> HelloWithErrorCodes()
    {
        BufferWriter w;
        w.WriteString("invoke test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString("");
        w.WriteBool(true);
        return w.Release();
    }

    json ListExtensions(EditorServer &server)
    {
        const Frame frame = Execute(server, CommandType::ListEditorExtensions);
        EXPECT_EQ(frame.type, EditorExtensionsType) << frame.Text();
        if (frame.type != EditorExtensionsType)
            return json();
        BufferReader r(frame.payload);
        return json::parse(r.ReadString());
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

    bool WaitUntil(const std::function<bool()> &condition, const std::chrono::milliseconds limit = 20s)
    {
        const auto end = std::chrono::steady_clock::now() + limit;
        while (!condition() && std::chrono::steady_clock::now() < end)
        {
            std::this_thread::sleep_for(5ms);
        }
        return condition();
    }

    class TempProject
    {
    public:
        TempProject()
        {
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-invoke-" + std::to_string(stamp) + "-" + std::to_string(counter++));
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

    // One extension with one menu item: `capabilities` is the Lua list's inside, `extra` more keys of the item, `body`
    // the body of run(ctx, p)
    std::string OneItemExtension(const std::string &name, const std::string &itemId, const std::string &capabilities,
                                 const std::string &body, const std::string &extra = "")
    {
        return "return Editor.Extension {\n"
               "  name = \"" + name + "\",\n"
               "  apiVersion = 1,\n"
               "  capabilities = { " + capabilities + " },\n"
               "  menu = {\n"
               "    { id = \"" + itemId + "\", path = \"Tools/" + name + "\"," + extra + "\n"
               "      run = function(ctx, p)\n" + body + "\n      end },\n"
               "  },\n"
               "}\n";
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
                _sensitivities.push_back(Logger::CurrentSensitivity());
            };
        }

        ~LogCapture() { Logger::logEvent -= _id; }

        LogCapture(const LogCapture &) = delete;
        LogCapture &operator=(const LogCapture &) = delete;

        [[nodiscard]] bool Has(const std::string_view text) const
        {
            const std::lock_guard lock(_mutex);
            for (const std::string &line : _lines)
            {
                if (line.find(text) != std::string::npos)
                    return true;
            }
            return false;
        }

        /// Whether one line holds both texts
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

        /// Whether every line holding `text` was logged at Admin sensitivity (and there is one)
        [[nodiscard]] bool OnlyAdminHas(const std::string_view text) const
        {
            const std::lock_guard lock(_mutex);
            bool found = false;
            for (std::size_t i = 0; i < _lines.size(); ++i)
            {
                if (_lines[i].find(text) == std::string::npos)
                    continue;
                found = true;
                if (_sensitivities[i] != Logger::LogSensitivity::Admin)
                    return false;
            }
            return found;
        }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        std::vector<Logger::LogSensitivity> _sensitivities;
        std::size_t _id = 0;
    };

    // What a test thread opens to let a worker that the invoke hook holds go on; shared with the hook
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool open = false;

        void Wait()
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return open; });
        }

        void Open()
        {
            {
                const std::lock_guard lock(mutex);
                open = true;
            }
            changed.notify_all();
        }
    };

    class ExtensionInvokeTest : public ::testing::Test
    {
    protected:
        void SetUp() override { _watchdog.emplace(std::chrono::seconds(120)); }

        void TearDown() override
        {
            ExtensionHost::SetInvokeHookForTest({});
            ExtensionHost::SetInvocationBudgetForTest(0, 0);
            ExtensionHost::SetInvokeGraceMillisForTest(-1);
            ExtensionHost::SetMaxNestedCommandsForTest(0);
            if (_gate)
            {
                _gate->Open();
            }
            EXPECT_TRUE(WaitUntil([] { return ExtensionHost::LiveWorkers() == 0; })) << "a worker never ended";
            _watchdog.reset();
        }

        // Starts `server` with the extensions of `project`, and a Hello that asks for error codes
        void Start(EditorServer &server, const TempProject &project) const
        {
            ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));
            ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
        }

        std::shared_ptr<Gate> _gate;
        std::optional<Tests::HangWatchdog> _watchdog;
    };

    const std::set<std::string> &AllowedNested()
    {
        static const std::set<std::string> allowed = {
            "GetHierarchy",   "GetEntity",           "GetComponentTypes",  "GetComponent",
            "CreateEntityEx", "DestroyEntity",       "DuplicateEntity",    "SetEntityParent",
            "SetEntityProperties", "SetLocalTransform", "AddComponent",    "RemoveComponent",
            "SetComponentFields",
        };
        return allowed;
    }
}

// ==================== What a script may send ====================

TEST(ExtensionNestedPolicyTest, EveryCommandOfTheSpecIsDecidedAndOnlyWhatEditorSceneNeedsIsAllowed)
{
    std::ifstream file(N2_PROTOCOL_JSON);
    ASSERT_TRUE(file.is_open()) << N2_PROTOCOL_JSON;
    const json spec = json::parse(file);

    std::set<std::string> allowed;
    std::size_t commands = 0;
    for (const auto &[name, command] : spec.at("commands").items())
    {
        ++commands;
        const auto id = static_cast<uint8_t>(std::stoul(command.at("id").get<std::string>(), nullptr, 16));
        const std::optional<bool> policy = EditorServer::NestedCommandPolicy(id);
        ASSERT_TRUE(policy.has_value()) << name << " is not decided for the nested dispatch";
        if (*policy)
            allowed.insert(name);
    }
    EXPECT_GT(commands, 60u);
    EXPECT_EQ(allowed, AllowedNested());

    // An id nobody listed (a free extension id, an event id, an unused one) is undecided, which the dispatch refuses
    for (const uint8_t id : {uint8_t{0x85}, uint8_t{0xC0}, uint8_t{0x7F}})
        EXPECT_FALSE(EditorServer::NestedCommandPolicy(id).has_value()) << int{id};

    // Each allowed one is a Write or a Read the editor role may run, never an Admin command
    for (const std::string &name : AllowedNested())
    {
        const auto id = static_cast<uint8_t>(std::stoul(spec.at("commands").at(name).at("id").get<std::string>(), nullptr, 16));
        const std::optional<EditorServer::CommandClass> commandClass = EditorServer::ClassifyCommand(id);
        ASSERT_TRUE(commandClass.has_value()) << name;
        EXPECT_NE(*commandClass, EditorServer::CommandClass::Admin) << name;
        EXPECT_FALSE(EditorServer::IsEditOnlyCommand(id)) << name;
    }
}

TEST(ExtensionNestedPolicyTest, EveryDeniedIdIsRefusedBeforeItRuns)
{
    LogCapture log;
    EditorServer server;
    const uint32_t revision = server.GetSceneRevision();
    int refused = 0;
    for (int value = 0; value <= 0xFF; ++value)
    {
        const auto id = static_cast<uint8_t>(value);
        if (EditorServer::NestedCommandPolicy(id).value_or(false))
            continue;
        // Shutdown, Undo, OpenScene, InvokeMenuItem ... and every id that is no command: none reaches a handler
        const Frame frame = Parse(server.DispatchNestedForTest(id, {}));
        EXPECT_EQ(frame.type, ErrorType) << int{id};
        EXPECT_EQ(frame.Text(), "An editor extension may not send " + EditorServer::CommandName(id)) << int{id};
        ++refused;
    }
    EXPECT_EQ(refused, 256 - static_cast<int>(AllowedNested().size()));
    EXPECT_EQ(server.GetSceneRevision(), revision);
    EXPECT_TRUE(log.Has("audit: extension test sent Shutdown, which an editor extension may not send (refused)"));
    EXPECT_TRUE(log.OnlyAdminHas("which an editor extension may not send"));

    // The host still answers, and an allowed read sent the same way runs (no scene: GetComponentTypes needs none)
    EXPECT_EQ(Execute(server, CommandType::GetComponentTypes).type, ComponentTypesType);
    EXPECT_EQ(Parse(server.DispatchNestedForTest(static_cast<uint8_t>(CommandType::GetComponentTypes), {})).type,
              ComponentTypesType);
    // An allowed command that needs a scene answers as it would a client
    const Frame hierarchy = Parse(server.DispatchNestedForTest(static_cast<uint8_t>(CommandType::GetHierarchy), {}));
    EXPECT_EQ(hierarchy.type, ErrorType);
    EXPECT_EQ(hierarchy.Text(), "No scene loaded");
}

// ==================== The gates ====================

TEST_F(ExtensionInvokeTest, AHostWithoutTheFlagRefusesWithTheExtensionsDisabledCode)
{
    EditorServer server;
    const Frame plain = Execute(server, CommandType::InvokeMenuItem, InvokePayload("a.item", json::array(), json::object(), 0));
    EXPECT_EQ(plain.type, ErrorType);
    EXPECT_NE(plain.Text().find("--editor-extensions"), std::string::npos) << plain.Text();

    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    const Coded coded =
        DecodeErrorCoded(Execute(server, CommandType::InvokeMenuItem, InvokePayload("a.item", json::array(), json::object(), 0)));
    EXPECT_EQ(coded.code, "extensionsDisabled");
}

TEST_F(ExtensionInvokeTest, AStaleUiRevisionIsRefusedAndNothingRuns)
{
    TempProject project;
    project.Write("a.lua", OneItemExtension("A", "a.item", "", "        Editor.Log.Info(\"ran\")"));
    EditorServer server;
    Start(server, project);
    const uint32_t current = server.GetEditorExtensions()->UiRevision();

    const Coded stale = DecodeErrorCoded(
        Execute(server, CommandType::InvokeMenuItem, InvokePayload("a.item", json::array(), json::object(), current + 7)));
    EXPECT_EQ(stale.code, "staleUiRevision");
    EXPECT_NE(stale.message.find("GetEditorUi"), std::string::npos) << stale.message;
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty()) << "the item ran";

    // The current revision runs it
    const Result ran = DecodeResult(Invoke(server, "a.item"));
    EXPECT_EQ(ran.label, "A");
    EXPECT_EQ(EventsOfKind(server, "extensionOutput").size(), 1u);
}

TEST_F(ExtensionInvokeTest, AnOpenClientEditGroupIsRefusedUntilItEnds)
{
    TempProject project;
    project.Write("a.lua", OneItemExtension("A", "a.item", "", "        Editor.Log.Info(\"ran\")"));
    EditorServer server;
    Start(server, project);

    BufferWriter label;
    label.WriteString("Drag");
    ASSERT_EQ(Execute(server, CommandType::BeginEditGroup, label.Release()).type, OkType);
    const Coded open = DecodeErrorCoded(Invoke(server, "a.item"));
    EXPECT_EQ(open.code, "editGroupOpen");
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty());

    ASSERT_EQ(Execute(server, CommandType::EndEditGroup).type, OkType);
    EXPECT_EQ(Invoke(server, "a.item").type, ExtensionResultType);
}

TEST_F(ExtensionInvokeTest, AReadOnlyHostRefusesTheInvocation)
{
    TempProject project;
    project.Write("a.lua", OneItemExtension("A", "a.item", "", "        Editor.Log.Info(\"ran\")"));
    EditorServer server;
    ASSERT_TRUE(server.SetReadOnly(true));
    ASSERT_TRUE(server.EnableEditorExtensions(project.Root()));

    const Frame plain = Invoke(server, "a.item");
    EXPECT_EQ(plain.type, ErrorType);
    EXPECT_EQ(plain.Text(), "read-only host: InvokeMenuItem is not allowed");
    ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
    EXPECT_EQ(DecodeErrorCoded(Invoke(server, "a.item")).code, "readOnly");
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty());
    // The lists still answer
    EXPECT_EQ(ListExtensions(server).at("extensions").size(), 1u);
}

TEST_F(ExtensionInvokeTest, TheTableClassifiesItAsAnEditOnlyWrite)
{
    const auto id = static_cast<uint8_t>(CommandType::InvokeMenuItem);
    EXPECT_EQ(EditorServer::ClassifyCommand(id), EditorServer::CommandClass::Write);
    EXPECT_FALSE(EditorServer::IsReadCommand(id));
    EXPECT_TRUE(EditorServer::IsEditOnlyCommand(id)) << "a play host refuses it (project/EditorExtensionPlayTests.cpp)";
    EXPECT_FALSE(EditorServer::RoleAllows(TokenRole::Viewer, id));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Editor, id));
    EXPECT_TRUE(EditorServer::RoleAllows(TokenRole::Admin, id));
}

TEST_F(ExtensionInvokeTest, AnUnknownItemABadSelectionAndBadParamsAreErrorsBeforeAnyLuaRuns)
{
    TempProject project;
    project.Write("a.lua", OneItemExtension("A", "a.item", "",
                                            "        Editor.Log.Info(\"ran\")\n"
                                            "        return { message = string.format(\"%s|%s|%d\", tostring(p.grid), "
                                            "tostring(p.snapY), #ctx.selection) }",
                                            "\n      params = { grid = { type = \"number\", default = 1.0, min = 0.5, max = 4 },"
                                            " snapY = { type = \"bool\", default = false } },"));
    EditorServer server;
    Start(server, project);

    const auto error = [&](const Frame &frame, const std::string &expected)
    {
        EXPECT_EQ(frame.type, ErrorType) << frame.Text();
        EXPECT_NE(frame.Text().find(expected), std::string::npos) << frame.Text();
    };
    error(Invoke(server, "no.such"), "Unknown menu item: no.such");
    error(Invoke(server, "a.item", json::object(), json{{"not", "a list"}}), "selection must be a JSON array");
    error(Invoke(server, "a.item", json::object(), json::array({"not-a-uuid"})), "UUID strings");
    error(Invoke(server, "a.item", json::object(), json::array({1})), "UUID strings");
    error(Invoke(server, "a.item", json::object(),
                 json(std::vector<std::string>(MaxInvokeSelection + 1, "01234567-89ab-cdef-0123-456789abcdef"))),
          "more than 4096");
    error(Invoke(server, "a.item", json{{"unknown", 1}}), "Unknown parameter 'unknown'");
    error(Invoke(server, "a.item", json{{"grid", "wide"}}), "grid");
    error(Invoke(server, "a.item", json::array({1, 2})), "params must be a JSON object");
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty()) << "a refused request ran the item";

    // Left out: the defaults. Out of range: clamped to the form's range. The selection reaches ctx.selection.
    EXPECT_EQ(DecodeResult(Invoke(server, "a.item")).result.at("message"), "1.0|false|0");
    const json two = json::array({"01234567-89ab-cdef-0123-456789abcdef", "fedcba98-7654-3210-fedc-ba9876543210"});
    EXPECT_EQ(DecodeResult(Invoke(server, "a.item", json{{"grid", 100.0}, {"snapY", true}}, two)).result.at("message"),
              "4.0|true|2");
    EXPECT_EQ(DecodeResult(Invoke(server, "a.item", json{{"grid", 0.0}})).result.at("message"), "0.5|false|0");
}

// ==================== Capabilities and the Editor namespace ====================

TEST_F(ExtensionInvokeTest, TheFunctionsOfAnUngrantedCapabilityAreAbsent)
{
    TempProject project;
    // No capability: no Editor.Scene at all; scene.read: the readers only; both: the writers too
    project.Write("a_none.lua", OneItemExtension("None", "none.item", "",
                                                 "        if Editor.Scene ~= nil then error(\"Editor.Scene exists\") end\n"
                                                 "        if type(Editor.Log.Info) ~= \"function\" then error(\"no log\") end"));
    project.Write("b_read.lua",
                  OneItemExtension("Read", "read.item", "\"scene.read\"",
                                   "        local S = Editor.Scene\n"
                                   "        for _, f in ipairs({ \"GetHierarchy\", \"GetEntity\", \"GetComponent\", "
                                   "\"GetLocalTransform\", \"FindByName\", \"GetComponentTypes\" }) do\n"
                                   "          if type(S[f]) ~= \"function\" then error(\"missing \" .. f) end\n"
                                   "        end\n"
                                   "        for _, f in ipairs({ \"CreateEntity\", \"Destroy\", \"Duplicate\", \"SetParent\", "
                                   "\"SetProperties\", \"SetLocalTransform\", \"AddComponent\", \"RemoveComponent\", "
                                   "\"SetFields\" }) do\n"
                                   "          if S[f] ~= nil then error(\"present \" .. f) end\n"
                                   "        end"));
    project.Write("c_write.lua",
                  OneItemExtension("Write", "write.item", "\"scene.read\", \"scene.write\"",
                                   "        local S = Editor.Scene\n"
                                   "        for _, f in ipairs({ \"GetHierarchy\", \"CreateEntity\", \"Destroy\", \"Duplicate\", "
                                   "\"SetParent\", \"SetProperties\", \"SetLocalTransform\", \"AddComponent\", "
                                   "\"RemoveComponent\", \"SetFields\" }) do\n"
                                   "          if type(S[f]) ~= \"function\" then error(\"missing \" .. f) end\n"
                                   "        end"));
    EditorServer server;
    Start(server, project);
    ASSERT_EQ(server.GetEditorExtensions()->LoadedCount(), 3u) << ListExtensions(server).dump();

    for (const char *item : {"none.item", "read.item", "write.item"})
    {
        const Frame frame = Invoke(server, item);
        EXPECT_EQ(frame.type, ExtensionResultType) << item << ": " << frame.Text();
    }
}

TEST_F(ExtensionInvokeTest, EditorLogAndPrintWriteExtensionOutputEventsThatOnlyAnAdminSees)
{
    TempProject project;
    project.Write("a.lua", "Editor.Log.Info(\"while loading\")\n" +
                               OneItemExtension("Log Ext", "log.item", "",
                                                "        Editor.Log.Info(\"hello\", 1, true)\n"
                                                "        Editor.Log.Warn(\"careful\")\n"
                                                "        Editor.Log.Error(\"bad\")\n"
                                                "        print(\"printed\", nil)"));
    LogCapture log;
    EditorServer server;
    Start(server, project);
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty()) << "a line written while the file loads is dropped";

    ASSERT_EQ(Invoke(server, "log.item").type, ExtensionResultType);
    const std::vector<json> events = EventsOfKind(server, "extensionOutput");
    ASSERT_EQ(events.size(), 4u);
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"info", "hello 1 true"}, {"warn", "careful"}, {"error", "bad"}, {"info", "printed nil"}};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(events[i].at("extension"), "Log Ext") << i;
        EXPECT_EQ(events[i].at("level"), expected[i].first) << i;
        EXPECT_EQ(events[i].at("message"), expected[i].second) << i;
        EXPECT_TRUE(events[i].contains("time")) << i;
    }
    // A role that reads the Public view sees none of it (audit F9): the output is Admin-tagged, as is its log line
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput", EventView::PublicOnly).empty());
    EXPECT_TRUE(log.OnlyAdminHas("extension Log Ext: hello 1 true"));
    EXPECT_TRUE(log.OnlyAdminHas("extension Log Ext: careful"));
    // The invocation's own audit line (no tokens file: no user or role to name)
    EXPECT_TRUE(log.HasLine("audit: user=- role=- extension=Log Ext@", "item=log.item"));
}

// ==================== Budgets and failures ====================

TEST_F(ExtensionInvokeTest, TheNestedCapEndsTheCallEvenWhenTheScriptCatchesTheError)
{
    ExtensionHost::SetMaxNestedCommandsForTest(5);
    TempProject project;
    project.Write("a.lua",
                  OneItemExtension("Cap", "cap.item", "\"scene.read\"",
                                   "        local n = (p.count or 0)\n"
                                   "        for i = 1, n do Editor.Scene.GetComponentTypes() end\n"
                                   "        if p.catch then\n"
                                   "          local ok = pcall(Editor.Scene.GetComponentTypes)\n"
                                   "          local again = pcall(Editor.Scene.GetComponentTypes)\n"
                                   "          if ok or again then error(\"the cap let a call through\") end\n"
                                   "        end\n"
                                   "        return { message = \"done\" }",
                                   "\n      params = { count = { type = \"int\", default = 0, min = 0, max = 100 },"
                                   " catch = { type = \"bool\" } },"));
    EditorServer server;
    Start(server, project);

    // At the cap: fine
    EXPECT_EQ(DecodeResult(Invoke(server, "cap.item", json{{"count", 5}})).result.at("message"), "done");
    // One past it: the call fails
    const Coded over = DecodeErrorCoded(Invoke(server, "cap.item", json{{"count", 6}}));
    EXPECT_EQ(over.code, "extensionFailed");
    EXPECT_NE(over.message.find("more than 5 commands"), std::string::npos) << over.message;
    // Caught by pcall, the cap still holds: every later call is refused too
    const Frame caught = Invoke(server, "cap.item", json{{"count", 5}, {"catch", true}});
    EXPECT_EQ(DecodeResult(caught).result.at("message"), "done");
}

TEST_F(ExtensionInvokeTest, ABudgetTripFaultsTheExtensionAndTheHostKeepsAnswering)
{
    ExtensionHost::SetInvocationBudgetForTest(200000, 0);
    TempProject project;
    project.Write("a.lua", OneItemExtension("Spin", "spin.item", "", "        while true do end"));
    project.Write("b.lua", OneItemExtension("Other", "other.item", "", "        return { message = \"fine\" }"));
    EditorServer server;
    Start(server, project);
    const uint32_t before = server.GetEditorExtensions()->UiRevision();

    const Coded tripped = DecodeErrorCoded(Invoke(server, "spin.item"));
    EXPECT_EQ(tripped.code, "extensionFailed");
    EXPECT_NE(tripped.message.find("disabled until reload"), std::string::npos) << tripped.message;
    EXPECT_FALSE(server.GetHistory().InGroup()) << "the group was closed";
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Faulted);
    EXPECT_EQ(server.GetEditorExtensions()->UiRevision(), before + 1);
    EXPECT_EQ(EventsOfKind(server, "extensionsChanged").size(), 1u);

    // The host answers, the other extension runs, and the faulted one is refused with its code
    EXPECT_EQ(ListExtensions(server).at("extensions")[0].at("state"), "faulted");
    EXPECT_EQ(DecodeResult(Invoke(server, "other.item")).result.at("message"), "fine");
    EXPECT_EQ(DecodeErrorCoded(Invoke(server, "spin.item")).code, "extensionFaulted");
    // A reload clears it
    ASSERT_EQ(Execute(server, CommandType::ReloadEditorExtensions).type, EditorExtensionsType);
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);
}

TEST_F(ExtensionInvokeTest, ThreeLuaErrorsFaultTheExtensionAndABadResultCountsToo)
{
    TempProject project;
    project.Write("a.lua", OneItemExtension("Err", "err.item", "",
                                            "        if p.bad then return { message = 5 } end\n"
                                            "        error(\"boom\")",
                                            "\n      params = { bad = { type = \"bool\" } },"));
    EditorServer server;
    Start(server, project);

    const Coded first = DecodeErrorCoded(Invoke(server, "err.item"));
    EXPECT_EQ(first.code, "extensionFailed");
    EXPECT_NE(first.message.find("a.lua:"), std::string::npos) << "the position is the file's own: " << first.message;
    EXPECT_NE(first.message.find("boom"), std::string::npos) << first.message;
    EXPECT_EQ(first.message.find("disabled"), std::string::npos) << first.message;
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Loaded);

    const Coded second = DecodeErrorCoded(Invoke(server, "err.item", json{{"bad", true}}));
    EXPECT_EQ(second.code, "extensionFailed");
    EXPECT_NE(second.message.find("not a result"), std::string::npos) << second.message;

    const Coded third = DecodeErrorCoded(Invoke(server, "err.item"));
    EXPECT_NE(third.message.find("disabled until reload"), std::string::npos) << third.message;
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("a.lua"), ExtensionState::Faulted);
}

TEST_F(ExtensionInvokeTest, AStalledWorkerIsAbandonedWithinTheDeadlineAndTheHostKeepsAnswering)
{
    ExtensionHost::SetInvocationBudgetForTest(0, 200);
    ExtensionHost::SetInvokeGraceMillisForTest(100);
    ASSERT_EQ(ExtensionHost::InvokeAbandonMillis(), 300u);
    _gate = std::make_shared<Gate>();
    const std::shared_ptr<Gate> gate = _gate;
    // Stands for a call into C that never returns: the worker is held before any Lua runs
    ExtensionHost::SetInvokeHookForTest([gate](const std::string &id)
    {
        if (id == "stuck.lua")
            gate->Wait();
    });

    TempProject project;
    project.Write("stuck.lua", OneItemExtension("Stuck", "stuck.item", "", "        return nil"));
    project.Write("z.lua", OneItemExtension("Fine", "fine.item", "", "        return { message = \"fine\" }"));
    EditorServer server;
    Start(server, project);

    const auto started = std::chrono::steady_clock::now();
    const Coded abandoned = DecodeErrorCoded(Invoke(server, "stuck.item"));
    const auto waited = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(abandoned.code, "extensionFailed");
    EXPECT_NE(abandoned.message.find("abandoned"), std::string::npos) << abandoned.message;
    EXPECT_GE(waited, 250ms);
    EXPECT_LT(waited, 5s) << "the host waited past the deadline";
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("stuck.lua"), ExtensionState::Faulted);
    EXPECT_EQ(ExtensionHost::LiveWorkers(), 1) << "the abandoned worker still holds its slot";

    // The host keeps answering while the worker is stuck
    EXPECT_EQ(ListExtensions(server).at("extensions")[0].at("state"), "faulted");
    EXPECT_EQ(DecodeResult(Invoke(server, "fine.item")).result.at("message"), "fine");
    EXPECT_EQ(DecodeErrorCoded(Invoke(server, "stuck.item")).code, "extensionFaulted");

    gate->Open();
    EXPECT_TRUE(WaitUntil([] { return ExtensionHost::LiveWorkers() == 0; }));
}

TEST_F(ExtensionInvokeTest, AnErrorNamesNoHostPathAndTheDetailIsAnAdminLogLine)
{
    TempProject project;
    const std::string root = project.Root().generic_string();
    project.Write("a.lua", OneItemExtension("Paths", "paths.item", "",
                                            "        error(\"cannot open [[" + root + "/assets/secret.txt]]\")"));
    // The same text with the folder written with backslashes
    std::string backslashes = root;
    for (char &c : backslashes)
        c = c == '/' ? '\\' : c;
    project.Write("b.lua", OneItemExtension("Back", "back.item", "",
                                            "        error([==[cannot open " + backslashes + "\\x.txt]==])"));
    LogCapture log;
    EditorServer server;
    server.SetCallerRemote(true);
    Start(server, project);

    for (const char *item : {"paths.item", "back.item"})
    {
        const Coded failed = DecodeErrorCoded(Invoke(server, item));
        EXPECT_EQ(failed.code, "extensionFailed") << item;
        EXPECT_EQ(failed.message.find(root), std::string::npos) << failed.message;
        EXPECT_EQ(failed.message.find(backslashes), std::string::npos) << failed.message;
        EXPECT_NE(failed.message.find("<project>"), std::string::npos) << failed.message;
    }
    EXPECT_TRUE(log.OnlyAdminHas("item paths.item failed"));
}

// ==================== Roles (a tokens file binds the role with Hello) ====================

namespace
{
    struct TokenUser
    {
        std::string name;
        std::string token;
        TokenRole role;
    };

    const TokenUser AdminUser{"ann", "ann-admin-token-0001", TokenRole::Admin};
    const TokenUser EditorUser{"ed", "ed-editor-token-0002", TokenRole::Editor};
    const TokenUser ViewerUser{"vic", "vic-viewer-token-0003", TokenRole::Viewer};

    bool WriteTokens(const fs::path &file)
    {
        std::string text = "[";
        bool first = true;
        for (const TokenUser &user : {AdminUser, EditorUser, ViewerUser})
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
        w.WriteString("invoke role test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString(user.token);
        w.WriteBool(true);
        w.WriteString(user.name);
        return w.Release();
    }

    class ExtensionInvokeRoleTest : public ExtensionInvokeTest
    {
    protected:
        void SetUp() override
        {
            ExtensionInvokeTest::SetUp();
            if (!EditorTlsServer::IsAvailable())
            {
                GTEST_SKIP() << "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";
            }
            _project.emplace();
            _project->Write("a.lua", OneItemExtension("Role Ext", "any.item", "\"scene.read\"",
                                                      "        Editor.Log.Info(\"ran\")\n"
                                                      "        return { message = \"ran\" }"));
            _project->Write("b.lua", OneItemExtension("Admin Ext", "admin.item", "", "        return { message = \"admin ran\" }",
                                                      "\n      minRole = \"admin\","));
            _tokens = _project->Root() / "tokens.json";
            ASSERT_TRUE(WriteTokens(_tokens));
            const auto set = server.SetTokensFile(_tokens);
            ASSERT_TRUE(set) << set.error();
            ASSERT_TRUE(server.EnableEditorExtensions(_project->Root()));
        }

        void As(const TokenUser &user)
        {
            ASSERT_EQ(Execute(server, CommandType::Hello, HelloAs(user)).type, ServerInfoType);
        }

        std::optional<TempProject> _project;
        fs::path _tokens;
        EditorServer server;
    };
}

TEST_F(ExtensionInvokeRoleTest, AViewerIsForbiddenAndNothingRuns)
{
    LogCapture log;
    As(ViewerUser);
    const Coded denied = DecodeErrorCoded(Invoke(server, "any.item"));
    EXPECT_EQ(denied.code, "forbidden");
    EXPECT_EQ(denied.message, "forbidden: role viewer is not allowed to run InvokeMenuItem");
    EXPECT_TRUE(EventsOfKind(server, "extensionOutput").empty());
    EXPECT_TRUE(log.Has("audit: denied user=vic role=viewer command=InvokeMenuItem"));

    // An editor may (D4), and the audit names the user, the role, the extension with its hash, and the item
    As(EditorUser);
    EXPECT_EQ(DecodeResult(Invoke(server, "any.item")).result.at("message"), "ran");
    EXPECT_TRUE(log.Has("audit: user=ed role=editor command=InvokeMenuItem"));
    EXPECT_TRUE(log.Has("audit: user=ed role=editor extension=Role Ext@"));
    EXPECT_TRUE(log.OnlyAdminHas("extension=Role Ext@"));
}

TEST_F(ExtensionInvokeRoleTest, AnItemWhoseMinRoleIsAdminIsForbiddenToAnEditor)
{
    As(EditorUser);
    const Coded denied = DecodeErrorCoded(Invoke(server, "admin.item"));
    EXPECT_EQ(denied.code, "forbidden");
    EXPECT_NE(denied.message.find("admin.item"), std::string::npos) << denied.message;

    As(AdminUser);
    EXPECT_EQ(DecodeResult(Invoke(server, "admin.item")).result.at("message"), "admin ran");
}

TEST_F(ExtensionInvokeRoleTest, ANestedCommandRunsWithTheInvokersRoleAndIsAuditedWithVia)
{
    LogCapture log;
    // The transform of an object of no scene: what matters is the gate in front of the handler
    BufferWriter transform;
    transform.WriteString("01234567-89ab-cdef-0123-456789abcdef");
    for (const float value : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f})
        transform.WriteF32(value);
    const std::vector<uint8_t> payload = transform.Release();
    const auto id = static_cast<uint8_t>(CommandType::SetLocalTransform);

    // A viewer's nested write is refused by the role gate, as the viewer's own would be
    As(ViewerUser);
    const Coded viewer = DecodeErrorCoded(Parse(server.DispatchNestedForTest(id, payload)));
    EXPECT_EQ(viewer.code, "forbidden");
    EXPECT_EQ(viewer.message, "forbidden: role viewer is not allowed to run SetLocalTransform");
    EXPECT_TRUE(log.HasLine("audit: denied user=vic role=viewer command=SetLocalTransform", "via=test"));

    // An editor's passes the gate (and the handler then answers: there is no scene), with the audit line naming both
    As(EditorUser);
    const Frame editor = Parse(server.DispatchNestedForTest(id, payload));
    EXPECT_EQ(editor.type, ErrorType) << editor.Text();
    EXPECT_EQ(editor.Text(), "No scene loaded");
    EXPECT_TRUE(log.HasLine("audit: user=ed role=editor command=SetLocalTransform", "via=test"));
    EXPECT_TRUE(log.OnlyAdminHas("via=test"));
}
