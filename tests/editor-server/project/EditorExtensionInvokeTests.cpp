#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditHistory.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/ExtensionHost.hpp>
#include <editor-server/Protocol.hpp>
#include <editor-server/Serialization.hpp>

#include "HangWatchdog.hpp"

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// InvokeMenuItem against a scene, really executed through ExecuteCommand: one invocation is one undo step (labelled with
// the item's title) that undo takes back and redo makes again; every Editor.Scene function reaches the scene through the
// command layer inside that one step; a failure (a Lua error, a budget trip) takes back every change with no history step
// and keeps the redo steps; a refused command the script catches keeps what was done; writes that restore the scene make
// no step; and the golden example of the design (Tools > Align > Snap selection to grid) end to end. Part of the
// editor_project_tests program, which owns SceneManager for its tests. The gates, roles and budgets without a scene are in
// ../ExtensionInvokeTests.cpp.
namespace
{
    constexpr uint8_t ErrorCodedType = static_cast<uint8_t>(ResponseType::ErrorCoded);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t HierarchyType = static_cast<uint8_t>(ResponseType::Hierarchy);
    constexpr uint8_t HistoryType = static_cast<uint8_t>(ResponseType::History);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
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

    std::vector<uint8_t> HelloWithErrorCodes()
    {
        BufferWriter w;
        w.WriteString("invoke scene test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString("");
        w.WriteBool(true);
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

    struct HistoryData
    {
        uint32_t cursor = 0;
        json entries;
    };

    struct Vec
    {
        double x = 0;
        double y = 0;
        double z = 0;
    };

    // The design's example (section 4.1), as an extension file would have it, with the selection it was given selected
    // again
    constexpr const char *GoldenExtension = R"LUA(
return Editor.Extension {
  name = "Align Tools",
  apiVersion = 1,
  capabilities = { "scene.read", "scene.write" },
  menu = {
    {
      id = "align.snapToGrid",
      path = "Tools/Align/Snap selection to grid",
      enabledWhen = { selection = "nonEmpty" },
      params = {
        grid = { type = "number", default = 1.0, min = 0.01, max = 100, tooltip = "Grid size in metres" },
        snapY = { type = "bool", default = false },
      },
      run = function(ctx, p)
        for _, id in ipairs(ctx.selection) do
          local t = Editor.Scene.GetLocalTransform(id)
          local s = function(v) return math.floor(v / p.grid + 0.5) * p.grid end
          t.position = { x = s(t.position.x), y = p.snapY and s(t.position.y) or t.position.y, z = s(t.position.z) }
          Editor.Scene.SetLocalTransform(id, t)
        end
        Editor.Log.Info(string.format("snapped %d", #ctx.selection))
        return { message = string.format("Snapped %d objects", #ctx.selection), select = ctx.selection }
      end,
    },
  },
}
)LUA";

    // Every Editor.Scene function in one call: the 13 commands a script may send, each through the command layer
    constexpr const char *EveryFunctionExtension = R"LUA(
return Editor.Extension {
  name = "Every Function",
  apiVersion = 1,
  capabilities = { "scene.read", "scene.write" },
  menu = {
    {
      id = "every.run",
      path = "Tools/Every function",
      run = function(ctx, p)
        local S = Editor.Scene
        local before = #S.GetHierarchy()
        local parent = S.CreateEntity { name = "Parent" }
        local child = S.CreateEntity { name = "Child", components = { "Light" } }
        S.SetParent(child, parent)
        S.SetProperties(parent, { name = "Renamed", active = false })
        local copy = S.Duplicate(child)
        local light
        for _, c in ipairs(S.GetEntity(child).components) do
          if c.type == "Light" then light = c.uuid end
        end
        if light == nil then error("the child has no Light") end
        S.SetFields(child, light, { intensity = 2.5 })
        if S.GetComponent(child, light).intensity ~= 2.5 then error("the field was not set") end
        local holder = S.CreateEntity { name = "Holder" }
        local added = S.AddComponent(holder, "Light", { range = 4 })
        if S.GetComponent(holder, added).range ~= 4 then error("the added component's value was not set") end
        S.RemoveComponent(holder, added)
        local t = S.GetLocalTransform(child)
        if t.scale.x ~= 1 then error("the scale is not 1") end
        S.SetLocalTransform(child, { position = { x = 1, y = 2, z = 3 } })
        if S.GetLocalTransform(child).position.y ~= 2 then error("the transform was not set") end
        if #S.FindByName("Renamed") ~= 1 then error("FindByName did not find the renamed object") end
        if #S.GetComponentTypes() == 0 then error("no component types") end
        local doomed = S.CreateEntity { name = "Doomed" }
        S.Destroy(doomed)
        local after = #S.GetHierarchy()
        if after - before ~= 4 then error(string.format("%d objects before, %d after", before, after)) end
        return { message = "ok", select = { parent, child, copy, holder }, warnings = { "a warning" } }
      end,
    },
  },
}
)LUA";

    // Moves the first selected object, then fails as `p.how` says
    constexpr const char *FailingExtension = R"LUA(
return Editor.Extension {
  name = "Failing",
  apiVersion = 1,
  capabilities = { "scene.read", "scene.write" },
  menu = {
    {
      id = "fail.run",
      path = "Tools/Fail",
      params = { how = { type = "enum", options = { "error", "spin", "refused", "caught", "restore" } } },
      run = function(ctx, p)
        local id = ctx.selection[1]
        local original = Editor.Scene.GetLocalTransform(id)
        Editor.Scene.SetLocalTransform(id, { position = { x = 50, y = 60, z = 70 } })
        Editor.Scene.CreateEntity { name = "Made by the failing item" }
        if p.how == "error" then
          error("boom")
        elseif p.how == "spin" then
          while true do end
        elseif p.how == "refused" then
          Editor.Scene.Destroy("00000000-0000-4000-8000-000000000000")
        elseif p.how == "caught" then
          local ok = pcall(Editor.Scene.Destroy, "00000000-0000-4000-8000-000000000000")
          if ok then error("the refused command succeeded") end
          return { message = "kept" }
        end
        return { message = "done" }
      end,
    },
    {
      id = "restore.run",
      path = "Tools/Restore",
      run = function(ctx, p)
        local id = ctx.selection[1]
        local original = Editor.Scene.GetLocalTransform(id)
        Editor.Scene.SetLocalTransform(id, { position = { x = 9, y = 9, z = 9 } })
        Editor.Scene.SetLocalTransform(id, original)
        return { message = "restored" }
      end,
    },
  },
}
)LUA";

    class EditorExtensionInvokeTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _watchdog.emplace(std::chrono::seconds(120));
            static std::atomic<int> counter{0};
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            _root = fs::temp_directory_path() / ("n2-ext-invoke-scene-" + std::to_string(stamp) + "-" +
                                                 std::to_string(counter++));
            fs::create_directories(_root / "editor");
            Write("align.lua", GoldenExtension);
            Write("every.lua", EveryFunctionExtension);
            Write("fail.lua", FailingExtension);

            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Invoke Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            server.GetHistory().SetCoalesceWindow(-1ms);
            ASSERT_TRUE(server.EnableEditorExtensions(_root));
            ASSERT_EQ(server.GetEditorExtensions()->LoadedCount(), 3u);
            ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
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

        Frame Invoke(const std::string &itemId, const json &selection, const json &params = json::object())
        {
            BufferWriter w;
            w.WriteString(itemId);
            WriteJson(w, selection);
            WriteJson(w, params);
            w.WriteU32(server.GetEditorExtensions()->UiRevision());
            return Execute(server, CommandType::InvokeMenuItem, w.Release());
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

        void SetPosition(const std::string &id, const Vec &position)
        {
            BufferWriter w;
            w.WriteString(id);
            for (const double value : {position.x, position.y, position.z, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0})
                w.WriteF32(static_cast<float>(value));
            const Frame frame = Execute(server, CommandType::SetLocalTransform, w.Release());
            EXPECT_EQ(frame.type, OkType) << frame.Text();
        }

        json Entity(const std::string &id)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({id}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return json();
            BufferReader r(frame.payload);
            return json::parse(r.ReadString());
        }

        Vec Position(const std::string &id)
        {
            const json entity = Entity(id);
            if (!entity.contains("transform"))
                return {};
            const json &p = entity.at("transform").at("position");
            return {p.at("x").get<double>(), p.at("y").get<double>(), p.at("z").get<double>()};
        }

        std::size_t ObjectCount()
        {
            const Frame frame = Execute(server, CommandType::GetHierarchy);
            EXPECT_EQ(frame.type, HierarchyType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadU32();
            return ReadJson(r).size();
        }

        HistoryData History()
        {
            const Frame frame = Execute(server, CommandType::GetHistory);
            EXPECT_EQ(frame.type, HistoryType) << frame.Text();
            HistoryData history;
            BufferReader r(frame.payload);
            history.cursor = r.ReadU32();
            history.entries = ReadJson(r);
            return history;
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

        static void ExpectAt(const Vec &actual, const Vec &expected, const char *what)
        {
            EXPECT_NEAR(actual.x, expected.x, 1e-5) << what;
            EXPECT_NEAR(actual.y, expected.y, 1e-5) << what;
            EXPECT_NEAR(actual.z, expected.z, 1e-5) << what;
        }

        EditorServer server;
        fs::path _root;
        std::optional<Tests::HangWatchdog> _watchdog;
    };
}

TEST_F(EditorExtensionInvokeTest, TheGoldenSnapToGridIsOneUndoStepThatUndoAndRedoTakeBackAndMakeAgain)
{
    const std::string a = Create("A");
    const std::string b = Create("B");
    SetPosition(a, {1.2, 3.7, -0.4});
    SetPosition(b, {2.6, 0.2, 5.5});
    const HistoryData before = History();
    const uint32_t savedBefore = server.GetSavedRevision();
    const uint32_t revisionBefore = server.GetSceneRevision();
    const std::size_t historyEvents = EventCount("historyChanged");

    const Result snapped = DecodeResult(Invoke("align.snapToGrid", json::array({a, b})));
    EXPECT_EQ(snapped.label, "Snap selection to grid");
    EXPECT_TRUE(snapped.canUndo);
    EXPECT_FALSE(snapped.canRedo);
    EXPECT_GT(snapped.revision, revisionBefore);
    EXPECT_EQ(snapped.savedRevision, savedBefore) << "the scene is dirty: its revision moved, the saved one did not";
    EXPECT_EQ(snapped.result.at("message"), "Snapped 2 objects");
    EXPECT_EQ(snapped.result.at("select"), json::array({a, b}));
    EXPECT_FALSE(snapped.result.contains("warnings"));
    ExpectAt(Position(a), {1.0, 3.7, 0.0}, "a snapped (y kept)");
    ExpectAt(Position(b), {3.0, 0.2, 6.0}, "b snapped (y kept)");

    // Exactly one step, labelled with the item's title, and the event that says so
    const HistoryData after = History();
    ASSERT_EQ(after.entries.size(), before.entries.size() + 1);
    EXPECT_EQ(after.entries.back().at("label"), "Snap selection to grid");
    EXPECT_EQ(after.cursor, before.cursor + 1);
    EXPECT_GT(EventCount("historyChanged"), historyEvents);
    EXPECT_EQ(EventCount("extensionOutput"), 1u);

    // Undo takes back both objects at once; redo snaps both again
    EXPECT_EQ(UndoOrRedo(CommandType::Undo), "Snap selection to grid");
    ExpectAt(Position(a), {1.2, 3.7, -0.4}, "a restored");
    ExpectAt(Position(b), {2.6, 0.2, 5.5}, "b restored");
    EXPECT_EQ(UndoOrRedo(CommandType::Redo), "Snap selection to grid");
    ExpectAt(Position(a), {1.0, 3.7, 0.0}, "a snapped again");
    ExpectAt(Position(b), {3.0, 0.2, 6.0}, "b snapped again");

    // With the form's values: a half-metre grid that snaps y as well
    const Result half = DecodeResult(Invoke("align.snapToGrid", json::array({a}), json{{"grid", 0.5}, {"snapY", true}}));
    EXPECT_EQ(half.result.at("message"), "Snapped 1 objects");
    ExpectAt(Position(a), {1.0, 3.5, 0.0}, "a on the half-metre grid");
    EXPECT_EQ(History().entries.size(), before.entries.size() + 2);
}

TEST_F(EditorExtensionInvokeTest, EveryEditorSceneFunctionRunsInsideTheOneStep)
{
    const std::size_t objectsBefore = ObjectCount();
    const std::size_t stepsBefore = History().entries.size();

    const Result result = DecodeResult(Invoke("every.run", json::array()));
    EXPECT_EQ(result.label, "Every function");
    EXPECT_EQ(result.result.at("message"), "ok");
    ASSERT_EQ(result.result.at("select").size(), 4u);
    EXPECT_EQ(result.result.at("warnings"), json::array({"a warning"}));
    EXPECT_EQ(ObjectCount(), objectsBefore + 4);
    const HistoryData after = History();
    ASSERT_EQ(after.entries.size(), stepsBefore + 1) << after.entries.dump();
    EXPECT_EQ(after.entries.back().at("label"), "Every function");

    const std::string parent = result.result.at("select")[0].get<std::string>();
    const json renamed = Entity(parent);
    EXPECT_EQ(renamed.at("header").at("name"), "Renamed");
    EXPECT_FALSE(renamed.at("header").at("active").get<bool>());

    // One undo takes all of it back, one redo makes all of it again
    EXPECT_EQ(UndoOrRedo(CommandType::Undo), "Every function");
    EXPECT_EQ(ObjectCount(), objectsBefore);
    EXPECT_EQ(UndoOrRedo(CommandType::Redo), "Every function");
    EXPECT_EQ(ObjectCount(), objectsBefore + 4);
    EXPECT_EQ(Entity(parent).at("header").at("name"), "Renamed");
}

TEST_F(EditorExtensionInvokeTest, ALuaErrorTakesBackEveryChangeWithNoStepAndKeepsTheRedoSteps)
{
    const std::string a = Create("A");
    SetPosition(a, {1.0, 2.0, 3.0});
    SetPosition(a, {4.0, 5.0, 6.0});
    ASSERT_EQ(UndoOrRedo(CommandType::Undo), "Transform A");
    const HistoryData before = History();
    ASSERT_TRUE(server.GetHistory().CanRedo());
    const std::size_t objectsBefore = ObjectCount();
    const uint32_t revisionBefore = server.GetSceneRevision();
    const uint32_t savedBefore = server.GetSavedRevision();
    const std::size_t sceneEvents = EventCount("sceneChanged");

    const Coded failed = DecodeErrorCoded(Invoke("fail.run", json::array({a}), json{{"how", "error"}}));
    EXPECT_EQ(failed.code, "extensionFailed");
    EXPECT_NE(failed.message.find("boom"), std::string::npos) << failed.message;
    EXPECT_NE(failed.message.find("taken back"), std::string::npos) << failed.message;

    ExpectAt(Position(a), {1.0, 2.0, 3.0}, "the move was taken back");
    EXPECT_EQ(ObjectCount(), objectsBefore) << "the object it made was taken back";
    const HistoryData after = History();
    EXPECT_EQ(after.entries, before.entries) << "no step was made";
    EXPECT_EQ(after.cursor, before.cursor);
    EXPECT_TRUE(server.GetHistory().CanRedo()) << "the redo step is still there";
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_GT(server.GetSceneRevision(), revisionBefore) << "the take-back is a change a client hears about";
    EXPECT_GT(EventCount("sceneChanged"), sceneEvents);
    EXPECT_EQ(server.GetSavedRevision(), savedBefore);
    EXPECT_EQ(UndoOrRedo(CommandType::Redo), "Transform A");
    ExpectAt(Position(a), {4.0, 5.0, 6.0}, "the redo step still works");
}

TEST_F(EditorExtensionInvokeTest, ABudgetTripAfterWritesIsTakenBackAndFaultsTheExtension)
{
    ExtensionHost::SetInvocationBudgetForTest(500000, 0);
    const std::string a = Create("A");
    SetPosition(a, {1.0, 2.0, 3.0});
    const std::size_t steps = History().entries.size();
    const std::size_t objects = ObjectCount();

    const Coded tripped = DecodeErrorCoded(Invoke("fail.run", json::array({a}), json{{"how", "spin"}}));
    EXPECT_EQ(tripped.code, "extensionFailed");
    EXPECT_NE(tripped.message.find("disabled until reload"), std::string::npos) << tripped.message;
    ExpectAt(Position(a), {1.0, 2.0, 3.0}, "taken back");
    EXPECT_EQ(ObjectCount(), objects);
    EXPECT_EQ(History().entries.size(), steps);
    EXPECT_FALSE(server.GetHistory().InGroup());
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("fail.lua"), ExtensionState::Faulted);
    EXPECT_EQ(DecodeErrorCoded(Invoke("fail.run", json::array({a}), json{{"how", "error"}})).code, "extensionFaulted");
    // The other extensions still run
    EXPECT_EQ(Invoke("align.snapToGrid", json::array({a})).type, ExtensionResultType);
}

TEST_F(EditorExtensionInvokeTest, ARefusedCommandFailsTheCallUnlessTheScriptCatchesIt)
{
    const std::string a = Create("A");
    SetPosition(a, {1.0, 2.0, 3.0});
    const std::size_t steps = History().entries.size();
    const std::size_t objects = ObjectCount();

    // Uncaught: a Lua error at the script's call, and everything is taken back
    const Coded refused = DecodeErrorCoded(Invoke("fail.run", json::array({a}), json{{"how", "refused"}}));
    EXPECT_EQ(refused.code, "extensionFailed");
    EXPECT_NE(refused.message.find("fail.lua:"), std::string::npos) << refused.message;
    ExpectAt(Position(a), {1.0, 2.0, 3.0}, "taken back");
    EXPECT_EQ(ObjectCount(), objects);
    EXPECT_EQ(History().entries.size(), steps);

    // Caught with pcall: the call goes on and keeps what it did, as one step
    const Result kept = DecodeResult(Invoke("fail.run", json::array({a}), json{{"how", "caught"}}));
    EXPECT_EQ(kept.result.at("message"), "kept");
    ExpectAt(Position(a), {50.0, 60.0, 70.0}, "kept");
    EXPECT_EQ(ObjectCount(), objects + 1);
    EXPECT_EQ(History().entries.size(), steps + 1);
}

TEST_F(EditorExtensionInvokeTest, WritesThatLeaveTheSceneAsItWasMakeNoStep)
{
    const std::string a = Create("A");
    SetPosition(a, {1.0, 2.0, 3.0});
    const std::size_t steps = History().entries.size();
    const bool couldUndo = server.GetHistory().CanUndo();

    const Result restored = DecodeResult(Invoke("restore.run", json::array({a})));
    EXPECT_EQ(restored.result.at("message"), "restored");
    EXPECT_EQ(restored.canUndo, couldUndo);
    ExpectAt(Position(a), {1.0, 2.0, 3.0}, "unchanged");
    EXPECT_EQ(History().entries.size(), steps) << "nothing to undo";
}

TEST_F(EditorExtensionInvokeTest, AnInvocationWithoutWritesMakesNoStep)
{
    const std::string a = Create("A");
    const std::size_t steps = History().entries.size();
    const uint32_t revision = server.GetSceneRevision();
    // An empty selection: the golden item reads and writes nothing
    const Result result = DecodeResult(Invoke("align.snapToGrid", json::array()));
    EXPECT_EQ(result.result.at("message"), "Snapped 0 objects");
    EXPECT_FALSE(result.result.contains("select")) << "an empty list is left out";
    EXPECT_EQ(History().entries.size(), steps);
    EXPECT_EQ(server.GetSceneRevision(), revision);
    (void)a;
}
