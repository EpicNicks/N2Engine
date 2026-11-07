#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
#include <editor-server/Protocol.hpp>
#include <engine/Application.hpp>
#include <engine/Time.hpp>
#include <engine/Window.hpp>
#include <engine/config/ApplicationOptions.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/SceneManager.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// GetEngineStats (EN5, protocol 1.28.0) through EditorServer::ExecuteCommand: an edit host (no frames, the scene's
// counts), a play host stepped while paused (one record per game frame, numbered as GetPlayState counts them, and
// sinceFrame), a RenderFrame's drawing and readback in the newest record, the idle switch-off, and the answer's shape
// against the golden vector. The play host is a second EditorServer in this process, as in EditorPlayTests.cpp; the
// frame timers belong to the one Application, so every test leaves them off.
namespace
{
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t PlaySnapshotType = static_cast<uint8_t>(ResponseType::PlaySnapshot);
    constexpr uint8_t FrameDataType = static_cast<uint8_t>(ResponseType::FrameData);
    constexpr uint8_t EngineStatsType = static_cast<uint8_t>(ResponseType::EngineStats);

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

    std::vector<uint8_t> U32Payload(const uint32_t value)
    {
        BufferWriter w;
        w.WriteU32(value);
        return w.Release();
    }

    std::vector<uint8_t> BoolPayload(const bool value)
    {
        BufferWriter w;
        w.WriteBool(value);
        return w.Release();
    }

    /// GetEngineStats {sinceFrame}, decoded
    json Stats(EditorServer &server, const uint32_t sinceFrame)
    {
        const Frame response = Execute(server, CommandType::GetEngineStats, U32Payload(sinceFrame));
        EXPECT_EQ(response.type, EngineStatsType) << response.Text();
        if (response.type != EngineStatsType)
            return json::object();
        BufferReader r(response.payload);
        json stats = ReadJson(r);
        EXPECT_FALSE(r.HasData()) << "trailing bytes after EngineStats";
        return stats;
    }

    std::vector<uint32_t> FrameNumbers(const json &stats)
    {
        std::vector<uint32_t> numbers;
        for (const json &record : stats.at("frames"))
            numbers.push_back(record.at("frame").get<uint32_t>());
        return numbers;
    }

    /// A project with a scene of three objects, a headless software renderer, and two servers: `server` edits, and `play`
    /// becomes the play host
    class EditorEngineStatsTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-stats-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "Stats Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "Stats Game");
            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));

            Config::ApplicationOptions options;
            options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            Window &window = Application::GetInstance().GetWindow();
            ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
            Application::GetInstance().GetFrameStats().SetEnabled(false);

            server.SetProject(_root, _project);
            const Frame made = Execute(server, CommandType::NewScene, Strings({"res://scenes/Stats.scene", "Stats Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            Create("First");
            Create("Second");
            Create("Third");
        }

        void TearDown() override
        {
            Application::GetInstance().GetFrameStats().SetEnabled(false);
            play.reset();
            IO::ResourceLoader::Instance().SetReadOnly(false);
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            Application::GetInstance().GetWindow().Shutdown();
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        void Create(const std::string &name)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("Empty");
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
        }

        /// Makes `play` a play host for the open scene's snapshot, paused, with a 16x12 viewport
        void StartPlayingPaused()
        {
            const Frame snapshot = Execute(server, CommandType::WritePlaySnapshot, Strings({""}));
            ASSERT_EQ(snapshot.type, PlaySnapshotType) << snapshot.Text();
            BufferReader r(snapshot.payload);
            const std::string file = r.ReadString();
            play = std::make_unique<EditorServer>();
            const auto entered = play->EnterPlayMode(fs::path(std::u8string(file.begin(), file.end())));
            ASSERT_TRUE(entered) << entered.error();
            BufferWriter size;
            size.WriteI32(16);
            size.WriteI32(12);
            ASSERT_EQ(Execute(*play, CommandType::SetViewportSize, size.Release()).type, OkType);
            ASSERT_EQ(Execute(*play, CommandType::SetPaused, BoolPayload(true)).type, OkType);
        }

        Frame Step(const uint32_t frames) { return Execute(*play, CommandType::Step, U32Payload(frames)); }

        EditorServer server;
        std::unique_ptr<EditorServer> play;

    private:
        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
    };
}

TEST(EditorEngineStatsProtocolTest, UsesProtocolIdsAndIsAPolledRead)
{
    EXPECT_EQ(static_cast<uint8_t>(CommandType::GetEngineStats), 0x54);
    EXPECT_EQ(static_cast<uint8_t>(ResponseType::EngineStats), 0x27);
    const auto id = static_cast<uint8_t>(CommandType::GetEngineStats);
    EXPECT_TRUE(EditorServer::IsReadCommand(id)) << "a viewer and a --read-only host answer it";
    EXPECT_TRUE(EditorServer::IsPolledCommand(id)) << "it never logs";
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(id)) << "a play host answers it";
    const std::vector<std::string> capabilities = EditorServer::Capabilities();
    EXPECT_NE(std::find(capabilities.begin(), capabilities.end(), "engineStats"), capabilities.end());
}

TEST_F(EditorEngineStatsTest, AnEditHostHasNoFramesAndCountsItsScene)
{
    const json stats = Stats(server, 0);

    EXPECT_EQ(stats.at("host").at("kind"), "edit");
    EXPECT_EQ(stats.at("host").at("state"), "Edit");
    EXPECT_EQ(stats.at("host").at("timersEnabled"), true) << "the first call turns the timers on";
    EXPECT_FALSE(stats.at("host").value("renderer", std::string{}).empty());
    EXPECT_EQ(stats.at("frame"), 0);
    EXPECT_TRUE(stats.at("frames").empty()) << "an edit host runs no game frames";
    EXPECT_EQ(stats.at("summary").at("windowFrames"), 0);

    EXPECT_EQ(stats.at("scene").at("objects"), 3);
    EXPECT_EQ(stats.at("scene").at("activeObjects"), 3);
    EXPECT_EQ(stats.at("scene").at("luaComponents"), 0);
    EXPECT_GE(stats.at("scene").at("components").get<int>(), 0);

    EXPECT_EQ(stats.at("render").at("counters"), true) << "the software renderer counts its draws";
    EXPECT_EQ(stats.at("render").at("width"), 1280) << "the default viewport";
    EXPECT_FALSE(stats.contains("physics")) << "no physics backend in this test";
}

TEST_F(EditorEngineStatsTest, APlayHostHasOneRecordPerGameFrameAndSinceFrameGivesTheNewerOnes)
{
    StartPlayingPaused();
    const json first = Stats(*play, 0);
    EXPECT_EQ(first.at("host").at("kind"), "play");
    EXPECT_EQ(first.at("host").at("state"), "Paused");
    EXPECT_TRUE(first.at("frames").empty()) << "the timers came on with this call";

    ASSERT_EQ(Step(5).type, OkType);
    const json stepped = Stats(*play, 0);
    EXPECT_EQ(FrameNumbers(stepped), (std::vector<uint32_t>{1, 2, 3, 4, 5})) << "numbered as GetPlayState counts them";
    EXPECT_EQ(stepped.at("frame"), 5);
    EXPECT_EQ(play->GetPlayFrame(), 5u);
    for (const json &record : stepped.at("frames"))
    {
        EXPECT_EQ(record.at("fixedTicks"), 1) << "a step is one fixed timestep";
        EXPECT_GE(record.at("tickMs").get<double>(), 0.0);
        EXPECT_EQ(record.at("renderMs"), 0) << "a play host draws on request";
    }
    EXPECT_EQ(stepped.at("summary").at("windowFrames"), 5);

    EXPECT_EQ(FrameNumbers(Stats(*play, 3)), (std::vector<uint32_t>{4, 5}));
    EXPECT_TRUE(Stats(*play, 5).at("frames").empty()) << "nothing newer";
    EXPECT_EQ(FrameNumbers(Stats(*play, 9000)).size(), 5u) << "a sinceFrame of another session gets everything";
}

TEST_F(EditorEngineStatsTest, ARenderFrameAfterAFrameIsTimedInThatFramesRecord)
{
    StartPlayingPaused();
    (void)Stats(*play, 0);
    ASSERT_EQ(Step(1).type, OkType);
    const json before = Stats(*play, 0);
    ASSERT_EQ(before.at("frames").size(), 1u);
    const auto framesRenderedBefore = before.at("render").at("framesRendered").get<uint64_t>();

    ASSERT_EQ(Execute(*play, CommandType::RenderFrame).type, FrameDataType);

    const json after = Stats(*play, 0);
    ASSERT_EQ(after.at("frames").size(), 1u) << "drawing is not a game frame";
    const json &record = after.at("frames")[0];
    EXPECT_GE(record.at("renderRequestMs").get<double>(), 0.0);
    EXPECT_GE(record.at("readbackMs").get<double>(), 0.0);
    EXPECT_GT(record.at("renderRequestMs").get<double>() + record.at("readbackMs").get<double>(), 0.0)
        << "the RenderFrame's drawing and readback went to the frame it followed";
    EXPECT_GE(record.at("commandsMs").get<double>() + 0.002,
              record.at("renderRequestMs").get<double>() + record.at("readbackMs").get<double>())
        << "the drawing and the readback are part of the command's time";
    EXPECT_EQ(after.at("render").at("framesRendered").get<uint64_t>(), framesRenderedBefore + 1);
    EXPECT_EQ(after.at("render").at("width"), 16);
    EXPECT_EQ(after.at("render").at("height"), 12);

    // A paused game's next RunPlayFrame closes the record: later commands count for no frame
    play->RunPlayFrame(std::chrono::milliseconds(0));
    const double commands = Stats(*play, 0).at("frames")[0].at("commandsMs").get<double>();
    ASSERT_EQ(Execute(*play, CommandType::RenderFrame).type, FrameDataType);
    EXPECT_EQ(Stats(*play, 0).at("frames")[0].at("commandsMs").get<double>(), commands);
}

TEST_F(EditorEngineStatsTest, TheTimersGoOffWhenNobodyHasAskedForAWhile)
{
    StartPlayingPaused();
    (void)Stats(*play, 0);
    auto &frameStats = Application::GetInstance().GetFrameStats();
    ASSERT_TRUE(frameStats.IsEnabled());

    const auto now = std::chrono::steady_clock::now();
    play->ExpireEngineStats(now + EditorServer::EngineStatsIdleTimeout - std::chrono::seconds(1));
    EXPECT_TRUE(frameStats.IsEnabled()) << "asked recently";
    play->ExpireEngineStats(now + EditorServer::EngineStatsIdleTimeout + std::chrono::seconds(1));
    EXPECT_FALSE(frameStats.IsEnabled());

    // Off: a frame is not recorded; the next call turns them on again, from nothing
    ASSERT_EQ(Step(2).type, OkType);
    const json again = Stats(*play, 0);
    EXPECT_TRUE(again.at("frames").empty());
    EXPECT_EQ(again.at("host").at("timersEnabled"), true);
}

TEST_F(EditorEngineStatsTest, APlayHostsAnswerHasTheShapeOfTheGoldenVector)
{
    std::ifstream vectorsFile(N2_PROTOCOL_TEST_VECTORS);
    ASSERT_TRUE(vectorsFile.good()) << N2_PROTOCOL_TEST_VECTORS;
    const json vectors = json::parse(vectorsFile);
    json golden;
    for (const json &variant : vectors.at("responseVariants"))
    {
        if (variant.at("response") == "EngineStats")
            golden = variant.at("fields").at("stats");
    }
    ASSERT_TRUE(golden.is_object()) << "no EngineStats variant in test-vectors.json";

    StartPlayingPaused();
    (void)Stats(*play, 0);
    ASSERT_EQ(Step(1).type, OkType);
    ASSERT_EQ(Execute(*play, CommandType::RenderFrame).type, FrameDataType);
    const json stats = Stats(*play, 0);

    for (const auto &[key, value] : stats.items())
    {
        ASSERT_TRUE(golden.contains(key)) << key << " is not in the golden vector";
        if (value.is_object())
        {
            for (const auto &[field, inner] : value.items())
            {
                (void)inner;
                EXPECT_TRUE(golden.at(key).contains(field)) << key << "." << field;
            }
            if (key != "physics" && key != "process" && key != "host")
            {
                // Blocks with no optional key: exactly the vector's
                EXPECT_EQ(value.size(), golden.at(key).size()) << key;
            }
        }
    }
    for (const char *always : {"host", "frame", "frames", "summary", "render", "scene"})
    {
        EXPECT_TRUE(stats.contains(always)) << always;
    }
    ASSERT_EQ(stats.at("frames").size(), 1u);
    for (const auto &[field, inner] : golden.at("frames")[0].items())
    {
        (void)inner;
        EXPECT_TRUE(stats.at("frames")[0].contains(field)) << "frames[]." << field;
    }
    EXPECT_EQ(stats.at("frames")[0].size(), golden.at("frames")[0].size());
    EXPECT_EQ(stats.at("summary").at("tickMs").size(), golden.at("summary").at("tickMs").size());
}
