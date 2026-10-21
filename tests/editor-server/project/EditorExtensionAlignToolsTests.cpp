#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
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
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

// The example extension of the repository, lua_project/editor/AlignTools.lua, loaded through the real ExtensionHost from
// where it lies (the project folder is lua_project itself, so the test reads the very file a user would copy) and run
// through InvokeMenuItem against a scene: its three items align, distribute and snap the selected objects as one undo step
// each, its form is checked by the host, and its OnSceneSaved hook runs read-only. It is also shown to be OFF by default:
// a host that was not given the flag has no extension host at all, and lists nothing. Part of the editor_project_tests
// program, which owns SceneManager for its tests. The host's own behaviour is in EditorExtensionInvokeTests.cpp.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t ServerInfoType = static_cast<uint8_t>(ResponseType::ServerInfo);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t EntityDataType = static_cast<uint8_t>(ResponseType::EntityData);
    constexpr uint8_t HistoryType = static_cast<uint8_t>(ResponseType::History);
    constexpr uint8_t EditResultType = static_cast<uint8_t>(ResponseType::EditResult);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t ExtensionResultType = static_cast<uint8_t>(ResponseType::ExtensionResult);
    constexpr uint8_t EditorExtensionsType = static_cast<uint8_t>(ResponseType::EditorExtensions);
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
        w.WriteString("align tools test");
        w.WriteString(std::string{ProtocolVersion});
        w.WriteString("");
        w.WriteBool(true);
        return w.Release();
    }

    struct Result
    {
        std::string label;
        bool canUndo = false;
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
        (void)r.ReadU32(); // revision
        decoded.canUndo = r.ReadBool();
        (void)r.ReadBool(); // canRedo
        (void)r.ReadU32();  // savedRevision
        decoded.result = ReadJson(r);
        EXPECT_FALSE(r.HasData());
        return decoded;
    }

    json DecodeJson(const Frame &frame, const uint8_t expectedType)
    {
        EXPECT_EQ(frame.type, expectedType) << frame.Text();
        if (frame.type != expectedType)
            return json();
        BufferReader r(frame.payload);
        const json value = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after the json field";
        return value;
    }

    struct Vec
    {
        double x = 0;
        double y = 0;
        double z = 0;
    };

    class AlignToolsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            _watchdog.emplace(std::chrono::seconds(120));
            const Frame made = Execute(server, CommandType::NewScene, Strings({"", "Align Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            server.GetHistory().SetCoalesceWindow(-1ms);
            // The project is lua_project itself: the host reads lua_project/editor/*.lua and nothing else
            ASSERT_TRUE(server.EnableEditorExtensions(fs::path(N2_LUA_PROJECT_DIR)));
            ASSERT_EQ(server.GetEditorExtensions()->LoadedCount(), 1u) << "AlignTools.lua did not load";
            ASSERT_EQ(Execute(server, CommandType::Hello, HelloWithErrorCodes()).type, ServerInfoType);
        }

        void TearDown() override
        {
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            (void)server.ShutdownEditorExtensions(std::chrono::milliseconds(2000));
            _watchdog.reset();
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

        std::string Create(const std::string &name, const Vec &position)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("");
            const Frame frame = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(frame.type, EntityCreatedType) << frame.Text();
            BufferReader r(frame.payload);
            const std::string id = r.ReadString();

            BufferWriter move;
            move.WriteString(id);
            for (const double value : {position.x, position.y, position.z, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0})
                move.WriteF32(static_cast<float>(value));
            const Frame moved = Execute(server, CommandType::SetLocalTransform, move.Release());
            EXPECT_EQ(moved.type, OkType) << moved.Text();
            return id;
        }

        Vec Position(const std::string &id)
        {
            const Frame frame = Execute(server, CommandType::GetEntity, Strings({id}));
            EXPECT_EQ(frame.type, EntityDataType) << frame.Text();
            if (frame.type != EntityDataType)
                return {};
            BufferReader r(frame.payload);
            const json entity = json::parse(r.ReadString());
            if (!entity.contains("transform"))
                return {};
            const json &p = entity.at("transform").at("position");
            return {p.at("x").get<double>(), p.at("y").get<double>(), p.at("z").get<double>()};
        }

        std::size_t HistorySteps()
        {
            const Frame frame = Execute(server, CommandType::GetHistory);
            EXPECT_EQ(frame.type, HistoryType) << frame.Text();
            BufferReader r(frame.payload);
            (void)r.ReadU32(); // the cursor
            return ReadJson(r).size();
        }

        std::string Undo()
        {
            const Frame frame = Execute(server, CommandType::Undo);
            EXPECT_EQ(frame.type, EditResultType) << frame.Text();
            if (frame.type != EditResultType)
                return {};
            BufferReader r(frame.payload);
            return r.ReadString();
        }

        std::vector<json> ExtensionOutput() const
        {
            std::vector<json> found;
            for (const json &event : server.GetEvents().Read(0, 4096, 0).events)
            {
                if (event.value("kind", "") == "extensionOutput")
                    found.push_back(event);
            }
            return found;
        }

        static void ExpectAt(const Vec &actual, const Vec &expected, const char *what)
        {
            EXPECT_NEAR(actual.x, expected.x, 1e-5) << what;
            EXPECT_NEAR(actual.y, expected.y, 1e-5) << what;
            EXPECT_NEAR(actual.z, expected.z, 1e-5) << what;
        }

        EditorServer server;
        std::optional<Tests::HangWatchdog> _watchdog;
    };
}

TEST_F(AlignToolsTest, TheFileLoadsThroughTheHostAndDeclaresItsThreeItemsAndOneHook)
{
    const json list = DecodeJson(Execute(server, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_TRUE(list.at("enabled").get<bool>());
    ASSERT_EQ(list.at("extensions").size(), 1u) << list.dump();
    const json &extension = list.at("extensions")[0];
    EXPECT_EQ(extension.at("id"), "AlignTools.lua");
    EXPECT_EQ(extension.at("name"), "Align Tools");
    EXPECT_EQ(extension.at("state"), "loaded");
    EXPECT_EQ(extension.at("apiVersion"), 1);
    EXPECT_EQ(extension.at("capabilities"), json::array({"scene.read", "scene.write"}));
    EXPECT_EQ(extension.at("itemCount"), 3);
    EXPECT_FALSE(extension.contains("error"));

    const json ui = DecodeJson(Execute(server, CommandType::GetEditorUi), EditorUiType);
    ASSERT_EQ(ui.at("menuItems").size(), 3u) << ui.dump();
    EXPECT_EQ(ui.at("menuItems")[0].at("id"), "align.align");
    EXPECT_EQ(ui.at("menuItems")[0].at("path"), "Tools/Align/Align selection");
    EXPECT_EQ(ui.at("menuItems")[1].at("id"), "align.distribute");
    EXPECT_EQ(ui.at("menuItems")[2].at("id"), "align.snapToGrid");
    EXPECT_EQ(ui.at("menuItems")[2].at("title"), "Snap selection to grid");
    for (const json &item : ui.at("menuItems"))
    {
        EXPECT_EQ(item.at("minRole"), "editor") << "no item asks for more than an editor";
        EXPECT_EQ(item.at("enabledWhen").at("selection"), "nonEmpty");
    }
    const json &snapDefaults = ui.at("menuItems")[2].at("form").at("defaults");
    EXPECT_NEAR(snapDefaults.at("grid").get<double>(), 1.0, 1e-9);
    EXPECT_TRUE(snapDefaults.at("snapX").get<bool>());
    EXPECT_FALSE(snapDefaults.at("snapY").get<bool>());
    EXPECT_TRUE(snapDefaults.at("snapZ").get<bool>());
    EXPECT_EQ(ui.at("menuItems")[0].at("form").at("defaults").at("axis"), "x");
    EXPECT_EQ(ui.at("menuItems")[0].at("form").at("defaults").at("mode"), "center");
    EXPECT_TRUE(ui.at("inspectorSections").empty());

    // The hook it declares is one the host calls: the host's own registry knows it
    EXPECT_EQ(server.GetEditorExtensions()->FindHooks("OnSceneSaved").size(), 1u);
    EXPECT_TRUE(server.GetEditorExtensions()->FindHooks("OnSceneOpened").empty());
}

TEST_F(AlignToolsTest, AlignMovesEverySelectedObjectOnTheAxisAsOneUndoStep)
{
    const std::string a = Create("A", {1.0, 2.0, 3.0});
    const std::string b = Create("B", {5.0, 0.0, 0.0});
    const std::string c = Create("C", {2.0, 9.0, 9.0});
    const json all = json::array({a, b, c});
    const std::size_t steps = HistorySteps();

    // The lowest x: every object goes to x = 1 and keeps its y and z
    const Result lowest = DecodeResult(Invoke("align.align", all, json{{"axis", "x"}, {"mode", "min"}}));
    EXPECT_EQ(lowest.label, "Align selection");
    EXPECT_TRUE(lowest.canUndo);
    EXPECT_EQ(lowest.result.at("message"), "Aligned 3 objects on x (min).");
    EXPECT_EQ(lowest.result.at("select"), all);
    ExpectAt(Position(a), {1.0, 2.0, 3.0}, "a was already at the lowest x");
    ExpectAt(Position(b), {1.0, 0.0, 0.0}, "b moved on x only");
    ExpectAt(Position(c), {1.0, 9.0, 9.0}, "c moved on x only");
    EXPECT_EQ(HistorySteps(), steps + 1) << "one invocation, one undo step";

    EXPECT_EQ(Undo(), "Align selection");
    ExpectAt(Position(b), {5.0, 0.0, 0.0}, "undo took back b");
    ExpectAt(Position(c), {2.0, 9.0, 9.0}, "undo took back c");

    // The highest y
    (void)DecodeResult(Invoke("align.align", all, json{{"axis", "y"}, {"mode", "max"}}));
    ExpectAt(Position(a), {1.0, 9.0, 3.0}, "a at the highest y");
    ExpectAt(Position(b), {5.0, 9.0, 0.0}, "b at the highest y");
    ExpectAt(Position(c), {2.0, 9.0, 9.0}, "c was already at the highest y");

    // The middle of z (the default mode is center): between 0 and 9
    const Result middle = DecodeResult(Invoke("align.align", all, json{{"axis", "z"}}));
    EXPECT_EQ(middle.result.at("message"), "Aligned 3 objects on z (center).");
    ExpectAt(Position(a), {1.0, 9.0, 4.5}, "a at the middle of z");
    ExpectAt(Position(b), {5.0, 9.0, 4.5}, "b at the middle of z");
    ExpectAt(Position(c), {2.0, 9.0, 4.5}, "c at the middle of z");
}

TEST_F(AlignToolsTest, AlignWithOneObjectChangesNothingAndMakesNoStep)
{
    const std::string a = Create("A", {1.0, 2.0, 3.0});
    const std::size_t steps = HistorySteps();
    const Result result = DecodeResult(Invoke("align.align", json::array({a})));
    EXPECT_EQ(result.result.at("message"), "Select two or more objects to align.");
    EXPECT_EQ(HistorySteps(), steps);
    ExpectAt(Position(a), {1.0, 2.0, 3.0}, "a did not move");
}

TEST_F(AlignToolsTest, DistributeSpreadsTheObjectsBetweenTheEndsInTheirOrderAlongTheAxis)
{
    const std::string a = Create("A", {1.0, 0.0, 0.0});
    const std::string b = Create("B", {5.0, 0.0, 0.0});
    const std::string c = Create("C", {2.0, 7.0, 0.0});
    const std::size_t steps = HistorySteps();

    // Selected in the order a, b, c; along x the order is a (1), c (2), b (5): c goes to the middle, 3
    const Result result = DecodeResult(Invoke("align.distribute", json::array({a, b, c}), json{{"axis", "x"}}));
    EXPECT_EQ(result.label, "Distribute selection evenly");
    EXPECT_EQ(result.result.at("message"), "Distributed 3 objects on x.");
    ExpectAt(Position(a), {1.0, 0.0, 0.0}, "the low end stays");
    ExpectAt(Position(b), {5.0, 0.0, 0.0}, "the high end stays");
    ExpectAt(Position(c), {3.0, 7.0, 0.0}, "c is in the middle and keeps its y");
    EXPECT_EQ(HistorySteps(), steps + 1);
    EXPECT_EQ(Undo(), "Distribute selection evenly");
    ExpectAt(Position(c), {2.0, 7.0, 0.0}, "undo took back c");

    // Two objects are not enough: a message, no change, no step
    const std::size_t before = HistorySteps();
    const Result two = DecodeResult(Invoke("align.distribute", json::array({a, b}), json{{"axis", "x"}}));
    EXPECT_EQ(two.result.at("message"), "Select three or more objects to distribute.");
    EXPECT_EQ(HistorySteps(), before);
}

TEST_F(AlignToolsTest, SnapToGridSnapsTheAxesTheFormAsksFor)
{
    const std::string a = Create("A", {1.2, 3.7, -0.4});
    const std::string b = Create("B", {2.6, 0.2, 5.5});
    const json both = json::array({a, b});

    // The form's defaults snap x and z and leave y: a half-metre grid
    const Result half = DecodeResult(Invoke("align.snapToGrid", both, json{{"grid", 0.5}}));
    EXPECT_EQ(half.label, "Snap selection to grid");
    EXPECT_EQ(half.result.at("message"), "Snapped 2 objects to a grid of 0.5.");
    ExpectAt(Position(a), {1.0, 3.7, -0.5}, "a on the half-metre grid, y kept");
    ExpectAt(Position(b), {2.5, 0.2, 5.5}, "b on the half-metre grid, y kept");

    // And with y asked for too, on the form's default grid of one metre: 3.7 goes to 4, and -0.5 rounds up to 0
    (void)DecodeResult(Invoke("align.snapToGrid", json::array({a}), json{{"snapY", true}}));
    ExpectAt(Position(a), {1.0, 4.0, 0.0}, "a on the one-metre grid on every axis");
    EXPECT_EQ(Undo(), "Snap selection to grid");
    ExpectAt(Position(a), {1.0, 3.7, -0.5}, "undo took back the second snap only");
}

TEST_F(AlignToolsTest, TheHostChecksTheFormBeforeTheFileRuns)
{
    const std::string a = Create("A", {1.0, 2.0, 3.0});
    const std::string b = Create("B", {5.0, 0.0, 0.0});
    const std::size_t steps = HistorySteps();

    // An axis that is not one of the options, and a name the form does not have: an Error, nothing run
    EXPECT_EQ(Invoke("align.align", json::array({a, b}), json{{"axis", "w"}}).type, ErrorType);
    EXPECT_EQ(Invoke("align.align", json::array({a, b}), json{{"depth", 1}}).type, ErrorType);
    EXPECT_EQ(HistorySteps(), steps);
    ExpectAt(Position(b), {5.0, 0.0, 0.0}, "nothing moved");
}

TEST_F(AlignToolsTest, ASelectionThatIsTooLargeIsRefusedWithAWarningBeforeAnythingIsRead)
{
    // 2001 ids that are not objects: the file looks at how many there are before it asks the host for any of them
    json ids = json::array();
    for (int i = 0; i < 2001; ++i)
    {
        char text[40];
        std::snprintf(text, sizeof(text), "00000000-0000-4000-8000-%012d", i);
        ids.push_back(std::string(text));
    }
    const Result result = DecodeResult(Invoke("align.align", ids));
    ASSERT_TRUE(result.result.contains("warnings")) << result.result.dump();
    EXPECT_EQ(result.result.at("warnings")[0], "Select at most 2000 objects (this selection has 2001).");
    EXPECT_FALSE(result.result.contains("message"));
}

TEST_F(AlignToolsTest, TheOnSceneSavedHookLogsAndFailsNothing)
{
    const std::size_t before = ExtensionOutput().size();
    EXPECT_EQ(server.RunExtensionHookForTest("OnSceneSaved", json{{"scene", "res://scenes/Demo.scene"}}), 0u);
    const std::vector<json> after = ExtensionOutput();
    ASSERT_EQ(after.size(), before + 1);
    EXPECT_EQ(after.back().at("extension"), "Align Tools");
    EXPECT_EQ(after.back().at("level"), "info");
    EXPECT_EQ(after.back().at("message"), "Align Tools: saved res://scenes/Demo.scene");
    EXPECT_EQ(server.GetEditorExtensions()->StateOf("AlignTools.lua"), ExtensionState::Loaded);
}

TEST(AlignToolsOffByDefaultTest, AHostThatWasNotGivenTheFlagHasNoExtensionHostAndListsNothing)
{
    // No EnableEditorExtensions: what Host.cpp does without --editor-extensions. lua_project's editor/ folder is never read.
    EditorServer plain;
    EXPECT_EQ(plain.GetEditorExtensions(), nullptr);

    const json list = DecodeJson(Execute(plain, CommandType::ListEditorExtensions), EditorExtensionsType);
    EXPECT_FALSE(list.at("enabled").get<bool>());
    EXPECT_TRUE(list.at("extensions").empty());
    const json ui = DecodeJson(Execute(plain, CommandType::GetEditorUi), EditorUiType);
    EXPECT_TRUE(ui.at("menuItems").empty());

    // And a call to run one of its items is refused, not run: the extensions are not enabled
    BufferWriter w;
    w.WriteString("align.align");
    WriteJson(w, json::array());
    WriteJson(w, json::object());
    w.WriteU32(0);
    const Frame refused = Execute(plain, CommandType::InvokeMenuItem, w.Release());
    EXPECT_NE(refused.type, ExtensionResultType) << "an item ran on a host without the flag";
}
