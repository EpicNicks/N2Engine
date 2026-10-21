#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditHistory.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/EditorTls.hpp>
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

// InvokeInspectorAction (protocol 1.16.0) against a scene, really executed through ExecuteCommand: a button of an
// inspector section runs on one component of its section's type as ONE undo step labelled with the button's label, with
// ctx.actionId, ctx.componentId and ctx.entityId; a failure (a Lua error, a budget trip) takes back every change with no
// step; a component that does not exist or is of another type is refused before any Lua runs; the edit-group, stale
// revision and faulted gates; and, with a tokens file, a viewer is forbidden, an admin-only button is forbidden to an
// editor, and the button's nested writes run with the invoker's role and are audited with via=. Part of the
// editor_project_tests program (it owns SceneManager). What needs no scene is in ../ExtensionInspectorTests.cpp.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t HierarchyType = static_cast<uint8_t>(ResponseType::Hierarchy);
    constexpr uint8_t HistoryType = static_cast<uint8_t>(ResponseType::History);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t ComponentAddedType = static_cast<uint8_t>(ResponseType::ComponentAdded);
    constexpr uint8_t ComponentDataType = static_cast<uint8_t>(ResponseType::ComponentData);
    constexpr uint8_t ExtensionResultType = static_cast<uint8_t>(ResponseType::ExtensionResult);

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
        EXPECT_FALSE(r.HasData());
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

        /// Whether one line holds every text
        [[nodiscard]] bool HasLine(std::initializer_list<std::string_view> texts) const
        {
            const std::lock_guard lock(_mutex);
            for (const std::string &line : _lines)
            {
                bool all = true;
                for (const std::string_view text : texts)
                    all = all && line.find(text) != std::string::npos;
                if (all)
                    return true;
            }
            return false;
        }

    private:
        mutable std::mutex _mutex;
        std::vector<std::string> _lines;
        std::size_t _id = 0;
    };

    // A section for Light with a field and three buttons: one that brightens the light and renames its object, one that
    // fails after writes (a Lua error, or a loop that trips the budget), and one for admins only
    constexpr const char *LightToolsExtension = R"LUA(
Editor.Inspector.Register("Light", {
  sections = {
    {
      label = "Light tools",
      fields = { intensity = { type = "number", min = 0, max = 100, tooltip = "How bright" } },
      buttons = {
        {
          id = "light.brighten",
          label = "Brighten",
          params = { by = { type = "number", default = 1, min = 0, max = 5 } },
          run = function(ctx, p)
            local c = Editor.Scene.GetComponent(ctx.entityId, ctx.componentId)
            Editor.Scene.SetFields(ctx.entityId, ctx.componentId, { intensity = c.intensity + p.by })
            Editor.Scene.SetProperties(ctx.entityId, { name = "Brightened" })
            Editor.Log.Info("brightened")
            return { message = ctx.actionId .. "|" .. ctx.componentId .. "|" .. ctx.entityId, select = { ctx.entityId } }
          end,
        },
        {
          id = "light.fail",
          label = "Fail",
          params = { how = { type = "enum", options = { "error", "spin" } } },
          run = function(ctx, p)
            Editor.Scene.SetFields(ctx.entityId, ctx.componentId, { intensity = 42 })
            Editor.Scene.CreateEntity { name = "Made by the failing button" }
            if p.how == "spin" then
              while true do end
            end
            error("button boom")
          end,
        },
        {
          id = "light.admin",
          label = "Admin only",
          minRole = "admin",
          run = function(ctx, p)
            return { message = "admin ran" }
          end,
        },
      },
    },
  },
})
return Editor.Extension { name = "Light Tools", apiVersion = 1, capabilities = { "scene.read", "scene.write" } }
)LUA";

    // Another extension's button on the same type: it still runs when Light Tools is faulted
    constexpr const char *OtherExtension = R"LUA(
Editor.Inspector.Register("Light", {
  sections = { { label = "Other", buttons = { { id = "other.ping", label = "Ping", run = function(ctx, p)
    return { message = "pong" }
  end } } } },
})
return Editor.Extension { name = "Other", apiVersion = 1, capabilities = { "scene.read" } }
)LUA";

    class InspectorSceneFixture : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _watchdog.emplace(std::chrono::seconds(120));
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-inspector-scene-" + std::to_string(stamp) + "-" +
                                                 std::to_string(counter++));
            fs::create_directories(_root / "editor");
            Write("light.lua", LightToolsExtension);
            Write("other.lua", OtherExtension);

            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Inspector Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            server.GetHistory().SetCoalesceWindow(-1ms);
        }

        void TearDown() override
        {
            ExtensionHost::SetInvocationBudgetForTest(0, 0);
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            (void)server.ShutdownEditorExtensions(std::chrono::milliseconds(2000));
            std::error_code error;
            fs::remove_all(_root, error);
            _watchdog.reset();
        }

        void Write(const std::string &relative, const std::string &text) const
        {
            std::ofstream file(_root / "editor" / relative, std::ios::binary | std::ios::trunc);
            file << text;
        }

        Frame Invoke(const std::string &componentId, const std::string &actionId, const json &params = json::object())
        {
            BufferWriter w;
            w.WriteString(componentId);
            w.WriteString(actionId);
            WriteJson(w, params);
            w.WriteU32(server.GetEditorExtensions()->UiRevision());
            return Execute(server, CommandType::InvokeInspectorAction, w.Release());
        }

        std::string Create(const std::string &name)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("");
            const Frame frame = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(frame.type, EntityCreatedType) << frame.Text();
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        std::string AddComponent(const std::string &entityId, const std::string &type)
        {
            const Frame frame = Execute(server, CommandType::AddComponent, Strings({entityId, type}));
            EXPECT_EQ(frame.type, ComponentAddedType) << frame.Text();
            if (frame.type != ComponentAddedType)
                return {};
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        double Intensity(const std::string &entityId, const std::string &componentId)
        {
            const Frame frame = Execute(server, CommandType::GetComponent, Strings({entityId, componentId}));
            EXPECT_EQ(frame.type, ComponentDataType) << frame.Text();
            if (frame.type != ComponentDataType)
                return -1.0;
            BufferReader r(frame.payload);
            return ReadJson(r).at("intensity").get<double>();
        }

        std::string NameOf(const std::string &entityId)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({entityId}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return {};
            BufferReader r(frame.payload);
            return json::parse(r.ReadString()).at("header").at("name").get<std::string>();
        }

        std::size_t ObjectCount()
        {
            const Frame frame = Execute(server, CommandType::GetHierarchy);
            EXPECT_EQ(frame.type, HierarchyType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadU32();
            return ReadJson(r).size();
        }

        json HistoryEntries()
        {
            const Frame frame = Execute(server, CommandType::GetHistory);
            EXPECT_EQ(frame.type, HistoryType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadU32();
            return ReadJson(r);
        }

        std::string UndoOrRedo(const CommandType type)
        {
            const Frame frame = Execute(server, type);
            EXPECT_EQ(frame.type, EditResultType) << frame.Text();
            if (frame.type != EditResultType)
                return {};
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        std::size_t EventCount(const std::string &kind) const
        {
            std::size_t count = 0;
            for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
                count += event.value("kind", "") == kind ? 1 : 0;
            return count;
        }

        EditorServer server;
        fs::path _root;
        std::optional<Tests::HangWatchdog> _watchdog;
    };

    // No tokens file: every client is an admin; Hello asks for error codes
    class EditorExtensionInspectorTest : public InspectorSceneFixture
    {
    protected:
        void SetUp() override
        {
            InspectorSceneFixture::SetUp();
            if (HasFatalFailure())
                return;
            ASSERT_TRUE(server.EnableEditorExtensions(_root));
            ASSERT_EQ(server.GetEditorExtensions()->LoadedCount(), 2u);
            BufferWriter w;
            w.WriteString("inspector scene test");
            w.WriteString(std::string{ProtocolVersion});
            w.WriteString("");
            w.WriteBool(true);
            ASSERT_EQ(Execute(server, CommandType::Hello, w.Release()).type, ServerInfoType);
        }
    };
}

TEST_F(EditorExtensionInspectorTest, AButtonRunsOnItsComponentAsOneUndoStepThatUndoAndRedoTakeBackAndMakeAgain)
{
    const std::string entity = Create("Lamp");
    const std::string light = AddComponent(entity, "Light");
    const double before = Intensity(entity, light);
    const std::size_t steps = HistoryEntries().size();
    const uint32_t savedBefore = server.GetSavedRevision();

    const Result brightened = DecodeResult(Invoke(light, "light.brighten", json{{"by", 2.0}}));
    EXPECT_EQ(brightened.label, "Brighten");
    EXPECT_TRUE(brightened.canUndo);
    EXPECT_FALSE(brightened.canRedo);
    EXPECT_EQ(brightened.savedRevision, savedBefore) << "the scene is dirty";
    // ctx carries the button, the component and the object that has it
    EXPECT_EQ(brightened.result.at("message"), "light.brighten|" + light + "|" + entity);
    EXPECT_EQ(brightened.result.at("select"), json::array({entity}));
    EXPECT_NEAR(Intensity(entity, light), before + 2.0, 1e-5);
    EXPECT_EQ(NameOf(entity), "Brightened");
    EXPECT_EQ(EventCount("extensionOutput"), 1u);

    // Exactly one step for both writes, labelled with the button's label
    const json after = HistoryEntries();
    ASSERT_EQ(after.size(), steps + 1);
    EXPECT_EQ(after.back().at("label"), "Brighten");

    EXPECT_EQ(UndoOrRedo(CommandType::Undo), "Brighten");
    EXPECT_NEAR(Intensity(entity, light), before, 1e-5);
    EXPECT_EQ(NameOf(entity), "Lamp");
    EXPECT_EQ(UndoOrRedo(CommandType::Redo), "Brighten");
    EXPECT_NEAR(Intensity(entity, light), before + 2.0, 1e-5);
    EXPECT_EQ(NameOf(entity), "Brightened");

    // The form's default, and a value clamped to its range (max 5)
    (void)DecodeResult(Invoke(light, "light.brighten"));
    EXPECT_NEAR(Intensity(entity, light), before + 3.0, 1e-5);
    (void)DecodeResult(Invoke(light, "light.brighten", json{{"by", 50.0}}));
    EXPECT_NEAR(Intensity(entity, light), before + 8.0, 1e-5);
    EXPECT_EQ(HistoryEntries().size(), steps + 3);
}

TEST_F(EditorExtensionInspectorTest, ALuaErrorTakesBackEveryChangeWithNoStep)
{
    const std::string entity = Create("Lamp");
    const std::string light = AddComponent(entity, "Light");
    const double before = Intensity(entity, light);
    const json steps = HistoryEntries();
    const std::size_t objects = ObjectCount();

    const Coded failed = DecodeErrorCoded(Invoke(light, "light.fail", json{{"how", "error"}}));
    EXPECT_EQ(failed.code, "extensionFailed");
    EXPECT_NE(failed.message.find("inspector button light.fail"), std::string::npos) << failed.message;
    EXPECT_NE(failed.message.find("button boom"), std::string::npos) << failed.message;
    EXPECT_NE(failed.message.find("taken back"), std::string::npos) << failed.message;

    EXPECT_NEAR(Intensity(entity, light), before, 1e-5) << "the field write was taken back";
    EXPECT_EQ(ObjectCount(), objects) << "the object it made was taken back";
    EXPECT_EQ(HistoryEntries(), steps) << "no step was made";
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("light.lua"), ExtensionState::Loaded) << "one failure does not fault";
}

TEST_F(EditorExtensionInspectorTest, ABudgetTripIsTakenBackFaultsTheExtensionAndTheHostKeepsAnswering)
{
    ExtensionHost::SetInvocationBudgetForTest(500000, 0);
    const std::string entity = Create("Lamp");
    const std::string light = AddComponent(entity, "Light");
    const double before = Intensity(entity, light);
    const std::size_t steps = HistoryEntries().size();
    const std::size_t objects = ObjectCount();

    const Coded tripped = DecodeErrorCoded(Invoke(light, "light.fail", json{{"how", "spin"}}));
    EXPECT_EQ(tripped.code, "extensionFailed");
    EXPECT_NE(tripped.message.find("disabled until reload"), std::string::npos) << tripped.message;
    EXPECT_NEAR(Intensity(entity, light), before, 1e-5);
    EXPECT_EQ(ObjectCount(), objects);
    EXPECT_EQ(HistoryEntries().size(), steps);
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("light.lua"), ExtensionState::Faulted);

    // The host answers, the faulted extension's buttons are refused with their code, the other extension's still run
    EXPECT_EQ(DecodeErrorCoded(Invoke(light, "light.brighten")).code, "extensionFaulted");
    EXPECT_EQ(DecodeResult(Invoke(light, "other.ping")).result.at("message"), "pong");
}

TEST_F(EditorExtensionInspectorTest, AComponentThatDoesNotExistOrIsOfAnotherTypeIsRefusedBeforeAnyLuaRuns)
{
    const std::string entity = Create("Lamp");
    const std::string light = AddComponent(entity, "Light");
    const std::string audio = AddComponent(entity, "AudioSource");
    const std::size_t steps = HistoryEntries().size();
    const uint32_t revision = server.GetSceneRevision();

    const auto error = [](const Frame &frame, const std::string &expected)
    {
        EXPECT_EQ(frame.type, ErrorType) << frame.Text();
        EXPECT_NE(frame.Text().find(expected), std::string::npos) << frame.Text();
    };
    error(Invoke("01234567-89ab-cdef-0123-456789abcdef", "light.brighten"), "Component not found");
    error(Invoke("not-a-uuid", "light.brighten"), "Component not found");
    error(Invoke(entity, "light.brighten"), "Component not found"); // an object's id is not a component's
    error(Invoke(audio, "light.brighten"), "is a AudioSource; the inspector button light.brighten is for a Light");
    error(Invoke(light, "no.such.button"), "Unknown inspector button: no.such.button");
    error(Invoke(light, "light.brighten", json{{"unknown", 1}}), "Unknown parameter 'unknown'");
    error(Invoke(light, "light.brighten", json{{"by", "lots"}}), "by");
    error(Invoke(light, "light.admin", json{{"by", 1}}), "takes no parameters");

    EXPECT_EQ(EventCount("extensionOutput"), 0u) << "a refused request ran the button";
    EXPECT_EQ(HistoryEntries().size(), steps);
    EXPECT_EQ(server.GetSceneRevision(), revision);
    EXPECT_FALSE(server.GetHistory().InGroup());
}

TEST_F(EditorExtensionInspectorTest, AStaleRevisionAndAnOpenEditGroupAreRefusedWithTheirCodes)
{
    const std::string entity = Create("Lamp");
    const std::string light = AddComponent(entity, "Light");

    BufferWriter stale;
    stale.WriteString(light);
    stale.WriteString("light.brighten");
    WriteJson(stale, json::object());
    stale.WriteU32(server.GetEditorExtensions()->UiRevision() + 3);
    const Coded refused = DecodeErrorCoded(Execute(server, CommandType::InvokeInspectorAction, stale.Release()));
    EXPECT_EQ(refused.code, "staleUiRevision");

    ASSERT_EQ(Execute(server, CommandType::BeginEditGroup, Strings({"Drag"})).type, OkType);
    const Coded open = DecodeErrorCoded(Invoke(light, "light.brighten"));
    EXPECT_EQ(open.code, "editGroupOpen");
    EXPECT_NE(open.message.find("inspector button"), std::string::npos) << open.message;
    ASSERT_EQ(Execute(server, CommandType::EndEditGroup).type, OkType);

    EXPECT_EQ(EventCount("extensionOutput"), 0u);
    EXPECT_EQ(Invoke(light, "light.brighten").type, ExtensionResultType);
}

TEST_F(EditorExtensionInspectorTest, TheInvocationIsAuditedWithTheButtonAndTheComponent)
{
    LogCapture log;
    const std::string entity = Create("Lamp");
    const std::string light = AddComponent(entity, "Light");
    ASSERT_EQ(Invoke(light, "light.brighten").type, ExtensionResultType);
    // No tokens file: no user or role to name
    EXPECT_TRUE(log.HasLine({"audit: user=- role=- extension=Light Tools@", "action=light.brighten",
                             "component=" + light}));
    EXPECT_TRUE(log.HasLine({"Editor extension Light Tools@", "action light.brighten finished"}));
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

    class EditorExtensionInspectorRoleTest : public InspectorSceneFixture
    {
    protected:
        void SetUp() override
        {
            InspectorSceneFixture::SetUp();
            if (HasFatalFailure())
                return;
            if (!EditorTlsServer::IsAvailable())
                GTEST_SKIP() << "a tokens file needs the TLS library's SHA-256 (N2ENGINE_EDITOR_TLS)";

            // The object and its light are made before the tokens file is in force (every command then needs a Hello)
            _entity = Create("Lamp");
            _light = AddComponent(_entity, "Light");

            std::string text = "[";
            bool first = true;
            for (const TokenUser &user : {AdminUser, EditorUser, ViewerUser})
            {
                const auto hash = HashToken(user.token);
                ASSERT_TRUE(hash.has_value());
                text += (first ? "" : ",\n ") + std::string("{\"user\":\"") + user.name + "\",\"role\":\"" +
                        std::string(TokenRoleName(user.role)) + "\",\"hash\":\"" + FormatTokenHash(*hash) + "\"}";
                first = false;
            }
            text += "]\n";
            _tokens = _root / "tokens.json";
            {
                std::ofstream out(_tokens, std::ios::binary | std::ios::trunc);
                out << text;
            }
#ifndef _WIN32
            fs::permissions(_tokens, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
            const auto set = server.SetTokensFile(_tokens);
            ASSERT_TRUE(set) << set.error();
            ASSERT_TRUE(server.EnableEditorExtensions(_root));
        }

        void TearDown() override
        {
            // The fixture's own clean-up (a NewScene) needs a role that may run it
            if (EditorTlsServer::IsAvailable() && !_tokens.empty())
                As(AdminUser);
            InspectorSceneFixture::TearDown();
        }

        void As(const TokenUser &user)
        {
            BufferWriter w;
            w.WriteString("inspector role test");
            w.WriteString(std::string{ProtocolVersion});
            w.WriteString(user.token);
            w.WriteBool(true);
            w.WriteString(user.name);
            ASSERT_EQ(Execute(server, CommandType::Hello, w.Release()).type, ServerInfoType);
        }

        std::string _entity;
        std::string _light;
        fs::path _tokens;
    };
}

TEST_F(EditorExtensionInspectorRoleTest, AViewerIsForbiddenAndNothingRuns)
{
    LogCapture log;
    As(ViewerUser);
    const Coded denied = DecodeErrorCoded(Invoke(_light, "light.brighten"));
    EXPECT_EQ(denied.code, "forbidden");
    EXPECT_EQ(denied.message, "forbidden: role viewer is not allowed to run InvokeInspectorAction");
    EXPECT_TRUE(log.HasLine({"audit: denied user=vic role=viewer command=InvokeInspectorAction"}));
    EXPECT_EQ(EventCount("extensionOutput"), 0u);
}

TEST_F(EditorExtensionInspectorRoleTest, AnEditorRunsTheButtonAndItsNestedWritesCarryTheEditorsRoleAndVia)
{
    LogCapture log;
    As(EditorUser);
    const double before = Intensity(_entity, _light);
    const Result result = DecodeResult(Invoke(_light, "light.brighten"));
    EXPECT_EQ(result.label, "Brighten");
    EXPECT_NEAR(Intensity(_entity, _light), before + 1.0, 1e-5);

    // The command, the invocation (who, which extension, which button, which component) and each nested write, which
    // ran as the editor (a tokens file audits every Write command) and names the extension it came from
    EXPECT_TRUE(log.HasLine({"audit: user=ed role=editor command=InvokeInspectorAction"}));
    EXPECT_TRUE(log.HasLine({"audit: user=ed role=editor extension=Light Tools@", "action=light.brighten",
                             "component=" + _light}));
    EXPECT_TRUE(log.HasLine({"audit: user=ed role=editor command=SetComponentFields", "via=Light_Tools"}));
    EXPECT_TRUE(log.HasLine({"audit: user=ed role=editor command=SetEntityProperties", "via=Light_Tools"}));
}

TEST_F(EditorExtensionInspectorRoleTest, AnAdminOnlyButtonIsForbiddenToAnEditor)
{
    As(EditorUser);
    const Coded denied = DecodeErrorCoded(Invoke(_light, "light.admin"));
    EXPECT_EQ(denied.code, "forbidden");
    EXPECT_EQ(denied.message, "forbidden: the inspector button light.admin may only be run by an admin (role editor)");

    As(AdminUser);
    EXPECT_EQ(DecodeResult(Invoke(_light, "light.admin")).result.at("message"), "admin ran");
}
