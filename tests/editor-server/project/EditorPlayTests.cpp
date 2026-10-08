#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <math/Vector2.hpp>
#include <nlohmann/json.hpp>

#include <editor-server/Commands.hpp>
#include <editor-server/EditorServer.hpp>
#include <editor-server/Protocol.hpp>
#include <engine/Application.hpp>
#include <engine/GameObjectScene.hpp>
#include <engine/Time.hpp>
#include <engine/Window.hpp>
#include <engine/common/Color.hpp>
#include <engine/config/ApplicationOptions.hpp>
#include <engine/input/InputTypes.hpp>
#include <engine/input/KeySource.hpp>
#include <engine/input/Mouse.hpp>
#include <engine/io/ProjectFile.hpp>
#include <engine/io/ResourceLoader.hpp>
#include <engine/io/ResourceUUID.hpp>
#include <engine/sceneManagement/Scene.hpp>
#include <engine/sceneManagement/SceneManager.hpp>
#include <engine/serialization/ComponentRegistry.hpp>
#include <engine/serialization/ComponentSerializer.hpp>

using namespace N2Engine;
using namespace N2Engine::Editor;
using namespace N2Engine::Editor::Protocol;
using nlohmann::json;
namespace fs = std::filesystem;

// Play mode (#82, E9) through EditorServer::ExecuteCommand: WritePlaySnapshot on the host that edits, and a play host
// (EnterPlayMode, RunPlayFrame) with SetPaused, Step, GetPlayState and SendInput, on a headless software renderer. The
// play host is a second EditorServer in this process (a real one is a second process, N2EditorHost --play, which the
// CI's play smoke test starts); the engine's state is shared, so the snapshot plays over the edit scene, as the scene
// manager has one current scene.
namespace
{
    constexpr uint8_t ErrorType = static_cast<uint8_t>(ResponseType::Error);
    constexpr uint8_t OkType = static_cast<uint8_t>(ResponseType::Ok);
    constexpr uint8_t SceneInfoType = static_cast<uint8_t>(ResponseType::SceneInfo);
    constexpr uint8_t EntityCreatedType = static_cast<uint8_t>(ResponseType::EntityCreated);
    constexpr uint8_t ComponentAddedType = static_cast<uint8_t>(ResponseType::ComponentAdded);
    constexpr uint8_t PlaySnapshotType = static_cast<uint8_t>(ResponseType::PlaySnapshot);
    constexpr uint8_t PlayStateType = static_cast<uint8_t>(ResponseType::PlayState);
    constexpr uint8_t FrameDataType = static_cast<uint8_t>(ResponseType::FrameData);

    constexpr const char *ProbeType = "EditorPlayTest_Probe";
    constexpr int ViewWidth = 16;
    constexpr int ViewHeight = 12;

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

    std::vector<uint8_t> JsonPayload(const json &value)
    {
        BufferWriter w;
        WriteJson(w, value);
        return w.Release();
    }

    std::vector<uint8_t> BoolPayload(const bool value)
    {
        BufferWriter w;
        w.WriteBool(value);
        return w.Release();
    }

    std::vector<uint8_t> U32Payload(const uint32_t value)
    {
        BufferWriter w;
        w.WriteU32(value);
        return w.Release();
    }

    std::string ReadFile(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        return text.str();
    }

    void WriteFile(const fs::path &path, const std::string &text)
    {
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    struct PlayState
    {
        std::string state;
        uint32_t frame = 0;
        float time = 0.0f;
    };

    PlayState GetPlayState(EditorServer &server)
    {
        const Frame response = Execute(server, CommandType::GetPlayState);
        EXPECT_EQ(response.type, PlayStateType) << response.Text();
        PlayState state;
        if (response.type != PlayStateType)
            return state;
        BufferReader r(response.payload);
        state.state = r.ReadString();
        state.frame = r.ReadU32();
        state.time = r.ReadF32();
        EXPECT_FALSE(r.HasData()) << "trailing bytes after PlayState";
        return state;
    }

    /// Events of one kind the server pushed since the ring began
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

    bool SceneHasObjectNamed(const std::string &name)
    {
        if (SceneManager::GetCurScene() == nullptr)
            return false;
        for (const auto &object : SceneManager::GetCurSceneRef().GetAllGameObjects())
        {
            if (object->GetName() == name)
                return true;
        }
        return false;
    }

    /// Counts the frame callbacks it gets, in the play scene
    class PlayProbe final : public SerializableComponent
    {
    public:
        explicit PlayProbe(GameObject &gameObject) : SerializableComponent(gameObject)
        {
            RegisterMember("marker", marker);
        }

        [[nodiscard]] std::string GetTypeName() const override { return ProbeType; }

        void OnUpdate() override { ++updates; }
        void OnFixedUpdate() override { ++fixedUpdates; }

        int marker = 7;
        static inline int updates = 0;
        static inline int fixedUpdates = 0;
    };

    /// A project with a scene "Play Test" (an object with a PlayProbe, saved) that then has an unsaved change (another
    /// object), a headless software renderer, and two servers: `server` edits, and `play` becomes the play host
    class EditorPlayTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ComponentRegistry::Instance().Register(
                ProbeType,
                [](GameObject &gameObject) -> std::unique_ptr<Component> { return std::make_unique<PlayProbe>(gameObject); });

            const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
            _base = fs::temp_directory_path() / "n2-editor-play-test" / info->name();
            std::error_code error;
            fs::remove_all(_base, error);

            const auto created = IO::CreateProject(_base / "Play Game");
            ASSERT_TRUE(created) << created.error().message;
            _project = *created;
            _root = fs::canonical(_base / "Play Game");
            IO::ResourceUUID::Initialize(_project.projectId);
            IO::ResourceLoader::Instance().Initialize(_root, _project.UserDataPath(_base / "user"));

            Config::ApplicationOptions options;
            options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
            options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
            options.isHeadless = true;
            Window &window = Application::GetInstance().GetWindow();
            ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
            window.clearColor = Common::Color::Red;
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            // Other tests of this program set both
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));

            server.SetProject(_root, _project);
            const Frame made = Execute(server, CommandType::NewScene, Strings({"res://scenes/Play.scene", "Play Test"}));
            ASSERT_EQ(made.type, SceneInfoType) << made.Text();
            const std::string probed = Create("Probed");
            const Frame added = Execute(server, CommandType::AddComponent, Strings({probed, ProbeType}));
            ASSERT_EQ(added.type, ComponentAddedType) << added.Text();
            ASSERT_EQ(Execute(server, CommandType::SaveSceneToFile, Strings({""})).type, SceneInfoType);
            _savedText = ReadFile(SceneFile());

            // An edit that isn't saved: it must play
            Create("Unsaved");

            PlayProbe::updates = 0;
            PlayProbe::fixedUpdates = 0;
        }

        void TearDown() override
        {
            // The play host's keyboard goes first, then the scene (its components release their resources while the
            // renderer is up), then the renderer
            play.reset();
            (void)Execute(server, CommandType::NewScene, Strings({"", "Empty"}));
            Application::GetInstance().GetWindow().Shutdown();
            std::error_code error;
            fs::remove_all(_base.parent_path(), error);
        }

        std::string Create(const std::string &name)
        {
            BufferWriter w;
            w.WriteString(name);
            w.WriteString("");
            w.WriteI32(-1);
            w.WriteString("Empty");
            const Frame created = Execute(server, CommandType::CreateEntityEx, w.Release());
            EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
            if (created.type != EntityCreatedType)
                return {};
            BufferReader r(created.payload);
            return r.ReadString();
        }

        [[nodiscard]] const fs::path &Root() const { return _root; }
        [[nodiscard]] fs::path SceneFile() const { return _root / "assets" / "scenes" / "Play.scene"; }
        [[nodiscard]] const std::string &SavedText() const { return _savedText; }

        /// WritePlaySnapshot's answer for a scene path (empty: the open scene)
        fs::path Snapshot(const std::string &scenePath = "")
        {
            const Frame response = Execute(server, CommandType::WritePlaySnapshot, Strings({scenePath}));
            EXPECT_EQ(response.type, PlaySnapshotType) << response.Text();
            if (response.type != PlaySnapshotType)
                return {};
            BufferReader r(response.payload);
            const std::string file = r.ReadString();
            EXPECT_FALSE(r.HasData()) << "trailing bytes after PlaySnapshot";
            return fs::path(std::u8string(file.begin(), file.end()));
        }

        /// Makes `play` a play host for the open scene's snapshot
        void StartPlaying()
        {
            const fs::path file = Snapshot();
            ASSERT_FALSE(file.empty());
            play = std::make_unique<EditorServer>();
            const auto entered = play->EnterPlayMode(file);
            ASSERT_TRUE(entered) << entered.error();
            BufferWriter size;
            size.WriteI32(ViewWidth);
            size.WriteI32(ViewHeight);
            ASSERT_EQ(Execute(*play, CommandType::SetViewportSize, size.Release()).type, OkType);
        }

        /// Runs `count` game frames of the clock's time, serving commands for no time at all in between
        void RunFrames(const int count)
        {
            for (int i = 0; i < count; ++i)
                play->RunPlayFrame(std::chrono::milliseconds(0));
        }

        Frame Send(const json &events) { return Execute(*play, CommandType::SendInput, JsonPayload(events)); }
        Frame Pause(const bool paused) { return Execute(*play, CommandType::SetPaused, BoolPayload(paused)); }
        Frame Step(const uint32_t frames) { return Execute(*play, CommandType::Step, U32Payload(frames)); }

        EditorServer server;
        std::unique_ptr<EditorServer> play;

    private:
        fs::path _base;
        fs::path _root;
        IO::ProjectFile _project;
        std::string _savedText;
    };
}

// ==================== WritePlaySnapshot ====================

TEST_F(EditorPlayTest, TheSnapshotOfTheOpenSceneIsTheSceneAsItIsInMemory)
{
    const fs::path file = Snapshot();

    ASSERT_TRUE(fs::is_regular_file(file)) << file.string();
    EXPECT_TRUE(file.is_absolute());
    EXPECT_TRUE(fs::equivalent(file.parent_path(), Root() / ".n2" / "play"));
    EXPECT_EQ(file.extension(), ".scene");
    EXPECT_EQ(file.filename().string(), "Play Test.scene");

    const json snapshot = json::parse(ReadFile(file));
    EXPECT_EQ(snapshot, SceneManager::GetCurSceneRef().Serialize()) << "what the editor has, saved or not";
    const std::string text = snapshot.dump();
    EXPECT_NE(text.find("Unsaved"), std::string::npos) << "the unsaved object plays";
    EXPECT_NE(text.find("Probed"), std::string::npos);
    EXPECT_NE(text.find(ProbeType), std::string::npos);
}

TEST_F(EditorPlayTest, WritingASnapshotChangesNothingInTheEditHost)
{
    const uint32_t revision = server.GetSceneRevision();
    const uint32_t savedRevision = server.GetSavedRevision();
    ASSERT_NE(revision, savedRevision) << "the scene has an unsaved object";
    const size_t historySteps = server.GetHistory().StepCount();
    const size_t sceneEvents = EventsOfKind(server, "sceneChanged").size();
    const size_t frameEvents = EventsOfKind(server, "frameChanged").size();
    const uint32_t frameRevision = server.GetFrameRevision();

    (void)Snapshot();
    (void)Snapshot();

    EXPECT_EQ(server.GetSceneRevision(), revision);
    EXPECT_EQ(server.GetSavedRevision(), savedRevision) << "still unsaved: a snapshot is not a save";
    EXPECT_EQ(server.GetHistory().StepCount(), historySteps);
    EXPECT_EQ(server.GetFrameRevision(), frameRevision);
    EXPECT_EQ(EventsOfKind(server, "sceneChanged").size(), sceneEvents);
    EXPECT_EQ(EventsOfKind(server, "frameChanged").size(), frameEvents);
    EXPECT_EQ(ReadFile(SceneFile()), SavedText()) << "the scene's own file isn't touched";
    EXPECT_TRUE(SceneManager::GetCurSceneRef().IsEditMode());
    EXPECT_FALSE(server.IsPlayMode());
}

TEST_F(EditorPlayTest, ASceneByPathIsSnapshotFromItsFileNotFromMemory)
{
    // Another scene becomes the open one, so Play.scene is a file and nothing else
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"res://scenes/Other.scene", "Other"})).type, SceneInfoType);

    const fs::path file = Snapshot("res://scenes/Play.scene");

    ASSERT_TRUE(fs::is_regular_file(file));
    EXPECT_EQ(json::parse(ReadFile(file)), json::parse(SavedText()));
    EXPECT_EQ(ReadFile(file).find("Unsaved"), std::string::npos);
    EXPECT_EQ(file.filename().string(), "Play.scene") << "named after the file's stem";
}

TEST_F(EditorPlayTest, ThePathOfTheOpenSceneMeansTheOpenSceneInMemory)
{
    const fs::path file = Snapshot("res://scenes/Play.scene");

    EXPECT_NE(ReadFile(file).find("Unsaved"), std::string::npos);
}

TEST_F(EditorPlayTest, TheSnapshotNameIsMadeSafeAndASecondSnapshotReplacesTheFirst)
{
    ASSERT_EQ(Execute(server, CommandType::NewScene, Strings({"", "Odd:/Name?"})).type, SceneInfoType);

    const fs::path first = Snapshot();
    EXPECT_EQ(first.filename().string(), "Odd__Name_.scene");
    const std::string before = ReadFile(first);

    Create("Added Later");
    const fs::path second = Snapshot();

    EXPECT_EQ(first, second);
    EXPECT_NE(ReadFile(second), before);
    EXPECT_NE(ReadFile(second).find("Added Later"), std::string::npos);
}

TEST_F(EditorPlayTest, ASnapshotRefusesWhatIsNotAScene)
{
    EXPECT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Missing.scene"})).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"res://../escape.scene"})).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Play.txt"})).type, ErrorType);
    EXPECT_EQ(Execute(server, CommandType::WritePlaySnapshot, Strings({"scenes/Play.scene"})).type, ErrorType);

    WriteFile(Root() / "assets" / "scenes" / "Broken.scene", "this is not json");
    const Frame broken = Execute(server, CommandType::WritePlaySnapshot, Strings({"res://scenes/Broken.scene"}));
    EXPECT_EQ(broken.type, ErrorType);
    EXPECT_NE(broken.Text().find("Not valid JSON"), std::string::npos) << broken.Text();
}

TEST(EditorPlaySnapshotTest, WithoutAProjectThereIsNowhereToWriteOne)
{
    EditorServer server;
    const Frame response = Execute(server, CommandType::WritePlaySnapshot, Strings({""}));
    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.Text().find("No project"), std::string::npos) << response.Text();
}

// ==================== Entering play mode ====================

TEST_F(EditorPlayTest, APlayHostPlaysTheSnapshotAsAGameAndReportsItsState)
{
    EXPECT_EQ(GetPlayState(server).state, "Edit") << "an ordinary host is not playing";
    EXPECT_EQ(GetPlayState(server).frame, 0u);

    StartPlaying();

    EXPECT_TRUE(play->IsPlayMode());
    EXPECT_FALSE(play->IsPaused());
    ASSERT_NE(SceneManager::GetCurScene(), nullptr);
    EXPECT_FALSE(SceneManager::GetCurSceneRef().IsEditMode()) << "a game, not an edit-mode scene";
    EXPECT_TRUE(SceneHasObjectNamed("Unsaved"));
    EXPECT_TRUE(SceneHasObjectNamed("Probed"));

    const PlayState state = GetPlayState(*play);
    EXPECT_EQ(state.state, "Playing");
    EXPECT_EQ(state.frame, 0u);

    const std::vector<json> started = EventsOfKind(*play, "playState");
    ASSERT_EQ(started.size(), 1u);
    EXPECT_EQ(started[0].value("state", ""), "Playing");
    EXPECT_EQ(started[0].value("frame", 99), 0);
}

TEST_F(EditorPlayTest, EachRunPlayFrameRunsOneGameFrame)
{
    StartPlaying();
    ASSERT_EQ(PlayProbe::updates, 0) << "components attach at the first frame, not when the scene loads";

    RunFrames(3);

    EXPECT_EQ(GetPlayState(*play).frame, 3u);
    EXPECT_EQ(PlayProbe::updates, 3);
    EXPECT_EQ(play->GetPlayFrame(), 3u);
}

TEST_F(EditorPlayTest, ARunPlayFrameTakesItsBudgetAndRunsOneFrameInIt)
{
    StartPlaying();

    const auto start = std::chrono::steady_clock::now();
    play->RunPlayFrame(std::chrono::milliseconds(40));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(elapsed, std::chrono::milliseconds(35)) << "the rest of the budget goes to serving commands";
    EXPECT_EQ(play->GetPlayFrame(), 1u);
}

TEST_F(EditorPlayTest, ASnapshotThatCantBePlayedLeavesTheServerAnOrdinaryHost)
{
    EditorServer host;
    const auto missing = host.EnterPlayMode(Root() / "nothing.scene");
    EXPECT_FALSE(missing);
    EXPECT_FALSE(host.IsPlayMode());

    WriteFile(Root() / "garbage.scene", "{ not json");
    const auto garbage = host.EnterPlayMode(Root() / "garbage.scene");
    ASSERT_FALSE(garbage);
    EXPECT_NE(garbage.error().find("Not valid JSON"), std::string::npos) << garbage.error();
    EXPECT_FALSE(host.IsPlayMode());
    EXPECT_EQ(GetPlayState(host).state, "Edit");
}

TEST_F(EditorPlayTest, ASecondEnterPlayModeIsRefused)
{
    StartPlaying();
    const auto again = play->EnterPlayMode(Snapshot());
    EXPECT_FALSE(again);
    EXPECT_TRUE(play->IsPlayMode());
}

TEST_F(EditorPlayTest, ThePlayHostsKeyboardGoesWithTheServer)
{
    StartPlaying();
    EXPECT_NE(Input::KeySource::Get(), nullptr);

    play.reset();

    EXPECT_EQ(Input::KeySource::Get(), nullptr);
}

// ==================== Pause and step ====================

TEST_F(EditorPlayTest, APausedGameRunsNoFramesAndSaysSoOnce)
{
    StartPlaying();
    RunFrames(2);
    ASSERT_EQ(PlayProbe::updates, 2);

    EXPECT_EQ(Pause(true).type, OkType);
    EXPECT_TRUE(play->IsPaused());
    RunFrames(5);
    EXPECT_EQ(PlayProbe::updates, 2) << "no frame while paused";
    EXPECT_EQ(GetPlayState(*play).frame, 2u);
    EXPECT_EQ(GetPlayState(*play).state, "Paused");

    EXPECT_EQ(Pause(true).type, OkType) << "pausing a paused game is not an error";
    const std::vector<json> events = EventsOfKind(*play, "playState");
    ASSERT_EQ(events.size(), 2u) << "started, paused: the second pause changed nothing";
    EXPECT_EQ(events[1].value("state", ""), "Paused");
    EXPECT_EQ(events[1].value("frame", 99), 2);

    EXPECT_EQ(Pause(false).type, OkType);
    EXPECT_EQ(GetPlayState(*play).state, "Playing");
    RunFrames(1);
    EXPECT_EQ(PlayProbe::updates, 3);
    ASSERT_EQ(EventsOfKind(*play, "playState").size(), 3u);
    EXPECT_EQ(EventsOfKind(*play, "playState")[2].value("state", ""), "Playing");
}

TEST_F(EditorPlayTest, ResumingDoesNotCountThePauseAsAFrame)
{
    StartPlaying();
    RunFrames(1);
    ASSERT_EQ(Pause(true).type, OkType);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ASSERT_EQ(Pause(false).type, OkType);
    RunFrames(1);

    EXPECT_LT(Time::GetUnscaledDeltaTime(), 0.15f) << "the first frame after a pause is a frame, not the pause";
}

TEST_F(EditorPlayTest, StepRunsOneFixedTimestepPerFrameWhilePaused)
{
    StartPlaying();
    RunFrames(1);
    ASSERT_EQ(Pause(true).type, OkType);
    const int updates = PlayProbe::updates;
    const int fixedUpdates = PlayProbe::fixedUpdates;
    const float timeBefore = GetPlayState(*play).time;

    EXPECT_EQ(Step(3).type, OkType);

    EXPECT_EQ(GetPlayState(*play).frame, 4u);
    EXPECT_EQ(PlayProbe::updates, updates + 3);
    EXPECT_EQ(PlayProbe::fixedUpdates, fixedUpdates + 3) << "each step is exactly one fixed update";
    const float step = static_cast<float>(Time::GetFixedTimestep());
    EXPECT_NEAR(GetPlayState(*play).time - timeBefore, 3.0f * step * Time::GetTimeScale(), 1e-4f);
    EXPECT_NEAR(Time::GetUnscaledDeltaTime(), step, 1e-6f);
    EXPECT_EQ(GetPlayState(*play).state, "Paused") << "stepping doesn't resume";
}

TEST_F(EditorPlayTest, StepRefusesAGameThatIsRunningAndFrameCountsOutOfRange)
{
    StartPlaying();
    RunFrames(1);

    const Frame running = Step(1);
    EXPECT_EQ(running.type, ErrorType);
    EXPECT_NE(running.Text().find("paused"), std::string::npos) << running.Text();

    ASSERT_EQ(Pause(true).type, OkType);
    const int updates = PlayProbe::updates;
    EXPECT_EQ(Step(0).type, ErrorType);
    EXPECT_EQ(Step(EditorServer::MaxStepFrames + 1).type, ErrorType);
    EXPECT_EQ(PlayProbe::updates, updates) << "a refused step runs nothing";
    EXPECT_EQ(Step(1).type, OkType);
    EXPECT_EQ(PlayProbe::updates, updates + 1);
}

TEST_F(EditorPlayTest, TheCommandsOfAPlayHostAreErrorsOnOneThatEdits)
{
    for (const CommandType type : {CommandType::SetPaused, CommandType::Step, CommandType::SendInput})
    {
        std::vector<uint8_t> payload;
        if (type == CommandType::SetPaused)
            payload = BoolPayload(true);
        else if (type == CommandType::Step)
            payload = U32Payload(1);
        else
            payload = JsonPayload(json::array());
        const Frame response = Execute(server, type, payload);
        EXPECT_EQ(response.type, ErrorType) << static_cast<int>(type);
        EXPECT_NE(response.Text().find("Not a play host"), std::string::npos) << response.Text();
    }
    EXPECT_FALSE(server.IsPlayMode());
}

// ==================== What a play host refuses ====================

TEST_F(EditorPlayTest, APlayHostRefusesTheCommandsThatWriteTheProjectOrSwapTheScene)
{
    StartPlaying();
    const std::string before = ReadFile(SceneFile());

    for (const CommandType type : {CommandType::OpenScene, CommandType::NewScene, CommandType::SaveSceneToFile,
                                   CommandType::DeleteScene, CommandType::SetProjectSettings,
                                   CommandType::SetStartupScene, CommandType::RestoreAutosave,
                                   CommandType::DiscardAutosave, CommandType::LoadScene})
    {
        EXPECT_TRUE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(type))) << static_cast<int>(type);
        const Frame response = Execute(*play, type);
        EXPECT_EQ(response.type, ErrorType) << static_cast<int>(type);
        EXPECT_NE(response.Text().find("play host"), std::string::npos) << response.Text();
    }
    EXPECT_FALSE(SceneManager::GetCurSceneRef().IsEditMode()) << "still the game";
    EXPECT_TRUE(SceneHasObjectNamed("Unsaved"));
    EXPECT_EQ(ReadFile(SceneFile()), before);
    EXPECT_FALSE(EditorServer::IsEditOnlyCommand(static_cast<uint8_t>(CommandType::GetHierarchy)));
}

TEST_F(EditorPlayTest, APlayHostWritesNoAutosave)
{
    StartPlaying();
    Frame created;
    {
        BufferWriter w;
        w.WriteString("Made In Play");
        w.WriteString("");
        w.WriteI32(-1);
        w.WriteString("Empty");
        created = Execute(*play, CommandType::CreateEntityEx, w.Release());
    }
    EXPECT_EQ(created.type, EntityCreatedType) << created.Text();
    play->SetAutosaveInterval(std::chrono::milliseconds(0));
    (void)play->ProcessCommands();

    // The edit host autosaved the scene's own file earlier; the play host's scene has none, so its autosave would be
    // the untitled one
    EXPECT_FALSE(fs::exists(play->GetAutosaveFile())) << "an edit of a running game is not an edit step";
}

// ==================== Frames ====================

TEST_F(EditorPlayTest, RenderFrameDrawsTheRunningGameWithoutRunningAFrameOrMovingItsClock)
{
    StartPlaying();
    RunFrames(2);
    const float time = Time::GetTime();
    const uint32_t frame = play->GetPlayFrame();
    const int updates = PlayProbe::updates;

    const Frame picture = Execute(*play, CommandType::RenderFrame);

    ASSERT_EQ(picture.type, FrameDataType) << picture.Text();
    BufferReader r(picture.payload);
    EXPECT_EQ(r.ReadU32(), static_cast<uint32_t>(ViewWidth));
    EXPECT_EQ(r.ReadU32(), static_cast<uint32_t>(ViewHeight));
    const auto pixels = r.ReadBytes(r.Remaining());
    ASSERT_EQ(pixels.size(), static_cast<size_t>(ViewWidth) * ViewHeight * 4);
    EXPECT_EQ(pixels[0], 255) << "the clear colour (red) where nothing is drawn";
    EXPECT_EQ(pixels[1], 0);
    EXPECT_EQ(pixels[3], 255);

    EXPECT_EQ(Time::GetTime(), time);
    EXPECT_EQ(play->GetPlayFrame(), frame);
    EXPECT_EQ(PlayProbe::updates, updates);
}

TEST_F(EditorPlayTest, ThePlayHostAnswersRenderFrameIfChangedWithAnErrorThatNamesRenderFrame)
{
    StartPlaying();
    BufferWriter w;
    w.WriteU32(0);

    const Frame response = Execute(*play, CommandType::RenderFrameIfChanged, w.Release());

    EXPECT_EQ(response.type, ErrorType);
    EXPECT_NE(response.Text().find("RenderFrame"), std::string::npos) << response.Text();
}

// ==================== Input ====================

TEST_F(EditorPlayTest, KeysAndMouseButtonsStayDownUntilTheyAreReleased)
{
    StartPlaying();
    const Input::KeySource *source = Input::KeySource::Get();
    ASSERT_NE(source, nullptr);
    EXPECT_FALSE(source->IsKeyDown(Input::Key::W));

    EXPECT_EQ(Send(json::parse(R"([{"type": "key", "key": "W", "down": true},
                                   {"type": "mouseButton", "button": "Right", "down": true}])")).type, OkType);
    RunFrames(3);
    EXPECT_TRUE(source->IsKeyDown(Input::Key::W)) << "frames don't release it";
    EXPECT_FALSE(source->IsKeyDown(Input::Key::A));
    EXPECT_TRUE(source->IsMouseButtonDown(Input::MouseButton::Right));
    EXPECT_FALSE(source->IsMouseButtonDown(Input::MouseButton::Left));

    EXPECT_EQ(Send(json::parse(R"([{"type": "key", "key": "W", "down": false}])")).type, OkType);
    EXPECT_FALSE(source->IsKeyDown(Input::Key::W));
    EXPECT_TRUE(source->IsMouseButtonDown(Input::MouseButton::Right));

    EXPECT_EQ(Send(json::parse(R"([{"type": "key", "key": "Space", "down": true}, {"type": "releaseAll"}])")).type,
              OkType);
    EXPECT_FALSE(source->IsKeyDown(Input::Key::Space)) << "releaseAll comes after the press, in order";
    EXPECT_FALSE(source->IsMouseButtonDown(Input::MouseButton::Right));
}

TEST_F(EditorPlayTest, ThePointerIsWhereItWasPutBeforeEveryFrame)
{
    StartPlaying();
    Input::Mouse *mouse = Input::Mouse::Get();
    ASSERT_NE(mouse, nullptr);

    EXPECT_EQ(Send(json::parse(R"([{"type": "pointer", "x": 10.5, "y": 4.0},
                                   {"type": "mouseButton", "button": "Left", "down": true},
                                   {"type": "scroll", "x": 0.0, "y": 2.0}])")).type, OkType);
    RunFrames(1);
    EXPECT_FLOAT_EQ(mouse->GetPosition().x, 10.5f);
    EXPECT_FLOAT_EQ(mouse->GetPosition().y, 4.0f);
    EXPECT_TRUE(mouse->GetButton(0));
    EXPECT_TRUE(mouse->GetButtonDown(0));
    EXPECT_FLOAT_EQ(mouse->GetScrollDelta().y, 2.0f);

    RunFrames(1);
    EXPECT_FLOAT_EQ(mouse->GetPosition().x, 10.5f) << "it stays";
    EXPECT_TRUE(mouse->GetButton(0));
    EXPECT_FALSE(mouse->GetButtonDown(0)) << "held, no longer an edge";
    EXPECT_FLOAT_EQ(mouse->GetScrollDelta().y, 0.0f) << "a scroll is one frame's";

    EXPECT_EQ(Send(json::parse(R"([{"type": "mouseButton", "button": "Left", "down": false}])")).type, OkType);
    RunFrames(1);
    EXPECT_FALSE(mouse->GetButton(0));
    EXPECT_TRUE(mouse->GetButtonUp(0));
}

TEST_F(EditorPlayTest, APausedGameTakesInputAndActsOnItAtItsNextFrame)
{
    StartPlaying();
    RunFrames(1);
    ASSERT_EQ(Pause(true).type, OkType);

    EXPECT_EQ(Send(json::parse(R"([{"type": "pointer", "x": 3.0, "y": 5.0}])")).type, OkType);
    EXPECT_EQ(Step(1).type, OkType);

    EXPECT_FLOAT_EQ(Input::Mouse::Get()->GetPosition().x, 3.0f);
    EXPECT_FLOAT_EQ(Input::Mouse::Get()->GetPosition().y, 5.0f);
}

TEST_F(EditorPlayTest, ABatchWithABadEventAppliesNoneOfIt)
{
    StartPlaying();
    const Input::KeySource *source = Input::KeySource::Get();

    const std::vector<std::string> bad = {
        R"({"type": "teleport"})",
        R"({"type": "key", "key": "NotAKey", "down": true})",
        R"({"type": "key", "key": "Unknown", "down": true})",
        R"({"type": "key", "key": "A"})",
        R"({"type": "key", "key": "A", "down": "yes"})",
        R"({"type": "key", "down": true})",
        R"({"type": "mouseButton", "button": "Seventh", "down": true})",
        R"({"type": "pointer", "x": 1.0})",
        R"({"type": "pointer", "x": "1", "y": 2.0})",
        R"({"type": "pointer", "x": null, "y": 2.0})",
        R"({"type": "scroll", "x": 1.0e12, "y": 0.0})",
        R"({"missing": "type"})",
        "5",
    };
    for (const std::string &eventText : bad)
    {
        // A valid key press first, then the bad event: the batch is checked whole before anything is applied
        const json batch = json::parse(R"([{"type": "key", "key": "W", "down": true}, )" + eventText + "]");
        const Frame response = Send(batch);
        EXPECT_EQ(response.type, ErrorType) << eventText;
        EXPECT_NE(response.Text().find("events[1]"), std::string::npos) << response.Text();
        EXPECT_FALSE(source->IsKeyDown(Input::Key::W)) << "applied before the bad event was found: " << eventText;
    }
}

TEST_F(EditorPlayTest, InputIsRefusedThatIsNotAnArrayOrTooLong)
{
    StartPlaying();

    EXPECT_EQ(Send(json::object()).type, ErrorType);
    EXPECT_EQ(Send("W").type, ErrorType);

    const json releaseAll = json::parse(R"({"type": "releaseAll"})");
    json tooMany = json::array();
    for (size_t i = 0; i <= EditorServer::MaxInputEvents; ++i)
        tooMany.push_back(releaseAll);
    EXPECT_EQ(Send(tooMany).type, ErrorType);

    json enough = json::array();
    for (size_t i = 0; i < EditorServer::MaxInputEvents; ++i)
        enough.push_back(releaseAll);
    EXPECT_EQ(Send(enough).type, OkType);
    EXPECT_EQ(Send(json::array()).type, OkType) << "an empty batch is fine";
}
