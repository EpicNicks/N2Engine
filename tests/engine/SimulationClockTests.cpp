#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/SimulationClock.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// The simulation clock (net-a2): the fixed-tick loop of Application::Tick, the 64-bit tick counter (Time::GetTick), and
// the network modes' timeScale-independent step, tick-boundary attach queue and no-silent-drop catch-up. The None
// mode's loop is pinned by a characterisation test whose constants were derived from the loop as it was before the
// clock existed (see NoneModeKeepsTheLoopOfBeforeTheClock).

using namespace N2Engine;

namespace
{
    // Exactly representable as a float and a double, so frame deltas that are multiples of it add up with no error and a
    // partition of the same time into different frames can be compared tick for tick
    constexpr double ExactStep = 1.0 / 64.0;

    /// Records every fixed tick it sees
    class ClockProbe final : public Component
    {
    public:
        explicit ClockProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "SimulationClockTest_Probe"; }

        void OnUpdate() override { ++updates; }
        void OnFixedUpdate() override
        {
            fixedTicks.push_back(Time::GetTick());
            fixedDeltas.push_back(Time::GetFixedDeltaTime());
        }

        static void Clear()
        {
            updates = 0;
            fixedTicks.clear();
            fixedDeltas.clear();
        }

        static inline int updates = 0;
        static inline std::vector<std::uint64_t> fixedTicks;
        static inline std::vector<float> fixedDeltas;
    };

    /// The object the spawner creates: records the ticks of its OnAttach and its FixedUpdates
    class ClockSpawned final : public Component
    {
    public:
        explicit ClockSpawned(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "SimulationClockTest_Spawned"; }

        void OnAttach() override { attachTicks.push_back(Time::GetTick()); }
        void OnFixedUpdate() override { fixedTicks.push_back(Time::GetTick()); }

        static void Clear()
        {
            attachTicks.clear();
            fixedTicks.clear();
        }

        static inline std::vector<std::uint64_t> attachTicks;
        static inline std::vector<std::uint64_t> fixedTicks;
    };

    /// Creates a ClockSpawned object during the fixed tick numbered SpawnTick (at its FixedUpdate)
    class ClockSpawner final : public Component
    {
    public:
        explicit ClockSpawner(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "SimulationClockTest_Spawner"; }

        void OnFixedUpdate() override
        {
            if (Time::GetTick() != SpawnTick)
            {
                return;
            }
            const auto object = GameObject::Create("Spawned");
            object->CreatePositionable();
            SceneManager::GetCurSceneRef().AddRootGameObject(object);
            object->AddComponent<ClockSpawned>();
        }

        static void Clear() { ClockSpawned::Clear(); }

        static inline std::uint64_t SpawnTick = 3;
    };
}

class SimulationClockTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Config::ApplicationOptions options;
        options.physicsBackend = Config::ApplicationOptions::PhysicsBackend::PHYSX;
        options.renderBackend = Config::ApplicationOptions::RenderBackend::SOFTWARE;
        options.isHeadless = true;
        Window &window = Application::GetInstance().GetWindow();
        ASSERT_TRUE(window.InitWindow(options)) << window.GetInitError();
        ASSERT_TRUE(window.SetRenderSize(8, 4));

        // No physics backend: the fixed step is FixedUpdate alone
        Application::GetInstance().Set3DPhysicsBackend(nullptr);

        Application::GetInstance().SetSimulationMode(SimulationMode::None);
        Application::GetInstance().GetSimulationClock().SetMaxCatchUpTicks(SimulationClock::DefaultMaxCatchUpTicks);
        Application::GetInstance().GetSimulationClock().SetBehindHandler({});
        Application::GetInstance().GetSimulationClock().Reset();
        Time::SetTimeScale(1.0f);
        ASSERT_TRUE(Time::SetFixedTimestep(0.02));

        SceneManager::AddScene(Scene::Create("SimulationClockTest_" + std::to_string(++sceneCounter)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        ASSERT_NE(SceneManager::GetCurScene(), nullptr);

        ClockProbe::Clear();
        ClockSpawner::Clear();
        ClockSpawner::SpawnTick = 3;
    }

    void TearDown() override
    {
        Application &app = Application::GetInstance();
        app.SetSimulationMode(SimulationMode::None);
        app.GetSimulationClock().SetMaxCatchUpTicks(SimulationClock::DefaultMaxCatchUpTicks);
        app.GetSimulationClock().SetBehindHandler({});
        app.GetSimulationClock().Reset();
        Time::SetTimeScale(1.0f);
        Time::SetFixedTimestep(0.02);

        // Other tests of this program rely on the engine clock standing still: a frame of no time leaves the frame
        // deltas at zero again
        TickOptions rest;
        rest.render = false;
        rest.deltaSeconds = 0.0;
        app.Tick(rest);

        SceneManager::AddScene(Scene::Create("SimulationClockTest_Empty_" + std::to_string(++sceneCounter)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        app.GetWindow().Shutdown();
    }

    /// A root object with a ClockProbe
    static void AddProbe()
    {
        const auto object = GameObject::Create("Probe");
        object->CreatePositionable();
        SceneManager::GetCurSceneRef().AddRootGameObject(object);
        object->AddComponent<ClockProbe>();
    }

    static void AddSpawner()
    {
        const auto object = GameObject::Create("Spawner");
        object->CreatePositionable();
        SceneManager::GetCurSceneRef().AddRootGameObject(object);
        object->AddComponent<ClockSpawner>();
    }

    /// One frame of exactly `seconds`, no drawing
    static void TickFor(const double seconds)
    {
        TickOptions options;
        options.render = false;
        options.deltaSeconds = seconds;
        Application::GetInstance().Tick(options);
    }

    static SimulationClock &Clock() { return Application::GetInstance().GetSimulationClock(); }

    static inline int sceneCounter = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// None mode: the loop as it was
// ---------------------------------------------------------------------------------------------------------------------

namespace
{
    // The frames fed to the loop (seconds). Includes a frame of no time, frames over the 0.25 s clamp (0.5, 0.3) and a
    // frame of exactly the clamp's neighbourhood (0.2).
    constexpr std::array<double, 10> CharacterisationDeltas = {0.016, 0.016, 0.016, 0.5, 0.0, 0.05, 0.3, 0.02, 0.2, 0.001};

    // Derived by running the loop as it was before SimulationClock existed (`_fixedTimestepAccumulator +=
    // min(float(delta), 0.25)`, then `while (acc >= float(step)) { if (fixedDt > 0) FixedUpdate; acc -= float(step); }`,
    // with float(step) = float(0.02)) on these deltas, in double arithmetic with the float roundings the engine does.
    // The step counts do not depend on the time scale except that a scale of 0 runs none; the accumulator never does.
    constexpr std::array<int, 10> StepsRunScaleNonZero = {0, 1, 1, 12, 0, 3, 12, 1, 10, 0};
    constexpr std::array<int, 10> StepsRunScaleZero = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    constexpr std::array<double, 10> AccumulatorAfterFrame = {
        0.01600000075995922,  0.012000001966953278, 0.008000003173947334, 0.018000008538365364, 0.018000008538365364,
        0.008000010624527931, 0.01800001598894596,  0.01800001598894596,  0.018000023439526558, 0.01900002348702401};

    struct ScaleCase
    {
        float scale;
        // The fixed delta a FixedUpdate sees: float(0.02 * scale) in double arithmetic, 0.02 being the unscaled one
        double expectedFixedDelta;
        const std::array<int, 10> &steps;
    };
}

TEST_F(SimulationClockTest, NoneModeKeepsTheLoopOfBeforeTheClock)
{
    AddProbe();
    const std::array<ScaleCase, 4> cases = {{
        {1.0f, 0.019999999552965164, StepsRunScaleNonZero},
        {0.5f, 0.009999999776482582, StepsRunScaleNonZero},
        {2.0f, 0.03999999910593033, StepsRunScaleNonZero},
        {0.0f, 0.0, StepsRunScaleZero},
    }};

    for (const ScaleCase &testCase : cases)
    {
        SCOPED_TRACE("timeScale " + std::to_string(testCase.scale));
        Clock().ResetAccumulator();
        Time::SetTimeScale(testCase.scale);
        ASSERT_EQ(Application::GetInstance().GetSimulationMode(), SimulationMode::None);

        for (std::size_t frame = 0; frame < CharacterisationDeltas.size(); ++frame)
        {
            SCOPED_TRACE("frame " + std::to_string(frame));
            ClockProbe::Clear();
            const std::uint64_t tickBefore = Time::GetTick();
            TickFor(CharacterisationDeltas[frame]);

            EXPECT_EQ(static_cast<int>(ClockProbe::fixedTicks.size()), testCase.steps[frame]);
            EXPECT_EQ(Time::GetTick() - tickBefore, static_cast<std::uint64_t>(testCase.steps[frame]));
            EXPECT_NEAR(Clock().GetAccumulator(), AccumulatorAfterFrame[frame], 1e-12);
            for (const float dt : ClockProbe::fixedDeltas)
            {
                EXPECT_EQ(dt, static_cast<float>(testCase.expectedFixedDelta));
            }
            EXPECT_EQ(ClockProbe::updates, 1);
        }
    }
}

TEST_F(SimulationClockTest, NoneModeDropsTheTimePastTheClampAndRunsNoLimitOfTicks)
{
    AddProbe();
    TickFor(10.0);

    // 0.25 s at 0.02 s: twelve steps, and the other 9.75 s are gone
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 12u);
    EXPECT_LT(Clock().GetAccumulator(), 0.02);
    EXPECT_FALSE(Clock().IsBehind());
}

TEST_F(SimulationClockTest, NoneModeFixedDeltaFollowsTheTimeScaleAndTimeScaleZeroPausesTheTickCounter)
{
    AddProbe();
    Time::SetTimeScale(0.5f);
    EXPECT_FLOAT_EQ(Time::GetFixedDeltaTime(), 0.01f);
    Time::SetTimeScale(0.0f);
    EXPECT_EQ(Time::GetFixedDeltaTime(), 0.0f);

    const std::uint64_t before = Time::GetTick();
    TickFor(0.1);
    EXPECT_EQ(Time::GetTick(), before) << "a paused None loop runs no ticks, so the counter stands still";
    EXPECT_TRUE(ClockProbe::fixedTicks.empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// The tick counter
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(SimulationClockTest, TheTickCounterIsZeroBeforeTheFirstTickAndGrowsByOnePerFixedTick)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    AddProbe();
    EXPECT_EQ(Time::GetTick(), 0u);

    TickFor(0.0);
    EXPECT_EQ(Time::GetTick(), 0u) << "a frame of no time runs no tick";

    TickFor(3 * ExactStep);
    // Inside tick N's FixedUpdate the counter is N
    ASSERT_EQ(ClockProbe::fixedTicks.size(), 3u);
    EXPECT_EQ(ClockProbe::fixedTicks[0], 1u);
    EXPECT_EQ(ClockProbe::fixedTicks[1], 2u);
    EXPECT_EQ(ClockProbe::fixedTicks[2], 3u);
    EXPECT_EQ(Time::GetTick(), 3u);

    TickFor(2 * ExactStep);
    EXPECT_EQ(ClockProbe::fixedTicks.back(), 5u);
    EXPECT_EQ(Time::GetTick(), 5u);
    for (std::size_t i = 1; i < ClockProbe::fixedTicks.size(); ++i)
    {
        EXPECT_EQ(ClockProbe::fixedTicks[i], ClockProbe::fixedTicks[i - 1] + 1) << "one increment per tick";
    }
}

TEST_F(SimulationClockTest, TheTickCounterGrowsTheSameWayInANetworkMode)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application::GetInstance().SetSimulationMode(SimulationMode::Authoritative);
    AddProbe();

    TickFor(4 * ExactStep);
    ASSERT_EQ(ClockProbe::fixedTicks.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(ClockProbe::fixedTicks[i], i + 1);
    }
    EXPECT_EQ(Time::GetTick(), 4u);
}

TEST_F(SimulationClockTest, ClockResetSetsTheCounterBackToZero)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    AddProbe();
    TickFor(2 * ExactStep);
    ASSERT_EQ(Time::GetTick(), 2u);

    Clock().Reset();
    EXPECT_EQ(Time::GetTick(), 0u);
    EXPECT_EQ(Clock().GetTick(), 0u);
    EXPECT_EQ(Clock().GetAccumulator(), 0.0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Network modes
// ---------------------------------------------------------------------------------------------------------------------

TEST_F(SimulationClockTest, ANetworkModeFixedDeltaIgnoresTheTimeScale)
{
    Application &app = Application::GetInstance();
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    AddProbe();
    app.SetSimulationMode(SimulationMode::Deterministic);
    const float step = Time::GetFixedUnscaledDeltaTime();

    for (const float scale : {1.0f, 0.5f, 2.0f, 0.0f})
    {
        SCOPED_TRACE("timeScale " + std::to_string(scale));
        Time::SetTimeScale(scale);
        EXPECT_EQ(Time::GetFixedDeltaTime(), step);

        ClockProbe::Clear();
        TickFor(3 * ExactStep);
        // Three ticks of the unscaled step, even paused: the time scale only scales what Update sees
        EXPECT_EQ(ClockProbe::fixedTicks.size(), 3u);
        for (const float dt : ClockProbe::fixedDeltas)
        {
            EXPECT_EQ(dt, step);
        }
        EXPECT_FLOAT_EQ(Time::GetDeltaTime(), static_cast<float>(Time::GetUnscaledDeltaTime() * scale));
        EXPECT_EQ(ClockProbe::updates, 1);
    }

    // Leaving the mode gives the time scale its say again
    Time::SetTimeScale(0.5f);
    app.SetSimulationMode(SimulationMode::None);
    EXPECT_FLOAT_EQ(Time::GetFixedDeltaTime(), step * 0.5f);
    Time::SetTimeScale(0.0f);
    EXPECT_EQ(Time::GetFixedDeltaTime(), 0.0f);
    app.SetSimulationMode(SimulationMode::Authoritative);
    EXPECT_EQ(Time::GetFixedDeltaTime(), step);
}

TEST_F(SimulationClockTest, ANetworkModeFixedDeltaFollowsAChangeOfTheFixedTimestep)
{
    Application::GetInstance().SetSimulationMode(SimulationMode::Authoritative);
    Time::SetTimeScale(0.25f);
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    EXPECT_EQ(Time::GetFixedDeltaTime(), static_cast<float>(ExactStep));
}

namespace
{
    struct PartitionResult
    {
        std::vector<std::uint64_t> probeTicks;
        std::vector<std::uint64_t> attachTicks;
        std::vector<std::uint64_t> spawnedFixedTicks;
        std::uint64_t finalTick = 0;

        bool operator==(const PartitionResult &) const = default;
    };
}

TEST_F(SimulationClockTest, AnObjectCreatedInATickJoinsAtTheNextTickBoundaryWhateverTheFrameTiming)
{
    // The same twelve ticks of time cut into frames in different ways; every cut must give the same tick-by-tick result
    const std::vector<std::vector<double>> partitions = {
        {12.0},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {5.0, 7.0},
        {3.0, 3.0, 3.0, 3.0},
        {0.0, 4.5, 0.0, 0.5, 7.0, 0.0},
        {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5},
    };

    std::vector<PartitionResult> results;
    for (const SimulationMode mode : {SimulationMode::Authoritative, SimulationMode::Deterministic})
    {
        for (const std::vector<double> &partition : partitions)
        {
            // A fresh scene per run
            SceneManager::AddScene(Scene::Create("SimulationClockTest_Run_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
            Application::GetInstance().SetSimulationMode(mode);
            Clock().Reset();
            ClockProbe::Clear();
            ClockSpawner::Clear();
            AddProbe();
            AddSpawner();

            for (const double steps : partition)
            {
                TickFor(steps * ExactStep);
            }

            PartitionResult result;
            result.probeTicks = ClockProbe::fixedTicks;
            result.attachTicks = ClockSpawned::attachTicks;
            result.spawnedFixedTicks = ClockSpawned::fixedTicks;
            result.finalTick = Time::GetTick();
            results.push_back(result);
        }
    }

    ASSERT_FALSE(results.empty());
    const PartitionResult &first = results.front();
    // Tick 1..12 ran; the object created in tick 3's FixedUpdate attaches at tick 4's boundary (counter already 4) and
    // takes part in ticks 4..12
    EXPECT_EQ(first.finalTick, 12u);
    ASSERT_EQ(first.attachTicks.size(), 1u);
    EXPECT_EQ(first.attachTicks[0], 4u);
    ASSERT_EQ(first.spawnedFixedTicks.size(), 9u);
    EXPECT_EQ(first.spawnedFixedTicks.front(), 4u);
    EXPECT_EQ(first.spawnedFixedTicks.back(), 12u);
    ASSERT_EQ(first.probeTicks.size(), 12u);
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        EXPECT_TRUE(results[i] == first) << "partition run " << i << " differs";
    }
}

#ifdef N2ENGINE_PHYSX_ENABLED
namespace
{
    /// At the FixedUpdate of tick SpawnTick creates a dynamic sphere (Rigidbody + SphereCollider) and keeps it in Ball;
    /// every FixedUpdate it records the tick and the ball's height, which only falls once the body is simulated
    class PhysXSpawnWatcher final : public Component
    {
    public:
        explicit PhysXSpawnWatcher(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "SimulationClockTest_PhysXWatcher"; }

        void OnFixedUpdate() override
        {
            if (Ball != nullptr)
            {
                ticks.push_back(Time::GetTick());
                heights.push_back(Ball->GetPositionable()->GetPosition().y);
            }
            if (Time::GetTick() == SpawnTick && Ball == nullptr)
            {
                Ball = GameObject::Create("PhysXBall");
                Ball->CreatePositionable();
                Ball->GetPositionable()->SetPosition(Math::Vector3(0.0f, 10.0f, 0.0f));
                SceneManager::GetCurSceneRef().AddRootGameObject(Ball);
                Ball->AddComponent<Physics::SphereCollider>()->SetRadius(0.5f);
                auto *body = Ball->AddComponent<Physics::Rigidbody>();
                body->SetBodyType(Physics::BodyType::Dynamic);
                body->SetGravityEnabled(true);
            }
        }

        static void Clear()
        {
            Ball.reset();
            ticks.clear();
            heights.clear();
        }

        static inline GameObject::Ptr Ball;
        static inline std::vector<std::uint64_t> ticks;
        static inline std::vector<float> heights;
        static inline std::uint64_t SpawnTick = 3;
    };
}

TEST_F(SimulationClockTest, APhysicsBodyCreatedInATickIsSimulatedFromTheNextTickBoundaryWhateverTheFrameTiming)
{
    const std::vector<std::vector<double>> partitions = {
        {12.0},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {5.0, 7.0},
        {0.0, 4.5, 0.0, 0.5, 7.0, 0.0},
    };

    std::vector<std::uint64_t> firstMovedTicks;
    for (const std::vector<double> &partition : partitions)
    {
        SceneManager::AddScene(Scene::Create("SimulationClockTest_PhysX_" + std::to_string(++sceneCounter)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        auto backend = std::make_unique<Physics::PhysXBackend>();
        ASSERT_TRUE(backend->Initialize());
        Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
        ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
        Application::GetInstance().SetSimulationMode(SimulationMode::Authoritative);
        Clock().Reset();
        PhysXSpawnWatcher::Clear();
        const auto watcher = GameObject::Create("Watcher");
        watcher->CreatePositionable();
        SceneManager::GetCurSceneRef().AddRootGameObject(watcher);
        watcher->AddComponent<PhysXSpawnWatcher>();

        for (const double steps : partition)
        {
            TickFor(steps * ExactStep);
        }
        EXPECT_EQ(Time::GetTick(), 12u);

        // The first tick whose FixedUpdate sees the ball lower than it started: the body was simulated by the tick
        // before it
        std::uint64_t firstMoved = 0;
        for (std::size_t i = 0; i < PhysXSpawnWatcher::ticks.size(); ++i)
        {
            if (PhysXSpawnWatcher::heights[i] < 10.0f)
            {
                firstMoved = PhysXSpawnWatcher::ticks[i];
                break;
            }
        }
        firstMovedTicks.push_back(firstMoved);

        // Release the body before its backend goes
        PhysXSpawnWatcher::Clear();
        SceneManager::AddScene(Scene::Create("SimulationClockTest_PhysX_Empty_" + std::to_string(++sceneCounter)), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
    }

    ASSERT_EQ(firstMovedTicks.size(), partitions.size());
    // Created in tick 3: attached and given its body at tick 4's boundary, stepped by tick 4, seen moved by tick 5
    EXPECT_GT(firstMovedTicks[0], PhysXSpawnWatcher::SpawnTick);
    EXPECT_LE(firstMovedTicks[0], PhysXSpawnWatcher::SpawnTick + 2);
    for (std::size_t i = 1; i < firstMovedTicks.size(); ++i)
    {
        EXPECT_EQ(firstMovedTicks[i], firstMovedTicks[0]) << "partition " << i << " differs";
    }
}
#endif // N2ENGINE_PHYSX_ENABLED

TEST_F(SimulationClockTest, ANetworkModeIgnoresAFrameDeltaThatIsNotTime)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application::GetInstance().SetSimulationMode(SimulationMode::Authoritative);
    Clock().SetMaxCatchUpTicks(0);
    AddProbe();

    // Straight to the clock: Time::Advance does not take these
    Clock().BeginFrame(std::numeric_limits<double>::quiet_NaN());
    Clock().BeginFrame(std::numeric_limits<double>::infinity());
    Clock().BeginFrame(-1.0);
    EXPECT_EQ(Clock().GetAccumulator(), 0.0);
    EXPECT_FALSE(Clock().TickDue(ExactStep));
    Clock().BeginFrame(2 * ExactStep);
    EXPECT_EQ(Clock().GetAccumulator(), 2 * ExactStep);
    Clock().ResetAccumulator();
}

TEST_F(SimulationClockTest, LeavingANetworkModeThatIsBehindKeepsOnlyTheNoneClampOfTheDebt)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application &app = Application::GetInstance();
    app.SetSimulationMode(SimulationMode::Authoritative);
    Clock().SetMaxCatchUpTicks(2);
    AddProbe();
    TickFor(640 * ExactStep);
    ASSERT_TRUE(Clock().IsBehind());

    app.SetSimulationMode(SimulationMode::None);
    EXPECT_LE(Clock().GetAccumulator(), SimulationClock::MaxFrameTime);
    ClockProbe::Clear();
    TickFor(0.0);
    EXPECT_LE(ClockProbe::fixedTicks.size(), 16u) << "0.25 s at 1/64 s, not the whole debt";
}

TEST_F(SimulationClockTest, AStandaloneClockDoesNotChangeTheApplicationsFixedDelta)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Time::SetTimeScale(0.5f);
    SimulationClock other;
    other.SetMode(SimulationMode::Authoritative);
    EXPECT_EQ(Application::GetInstance().GetSimulationMode(), SimulationMode::None);
    EXPECT_FLOAT_EQ(Time::GetFixedDeltaTime(), static_cast<float>(ExactStep) * 0.5f);
}

TEST_F(SimulationClockTest, InNoneModeTheAttachQueueRunsOncePerFrameBeforeTheFixedSteps)
{
    // The contrast: one frame of three steps attaches the spawner once, before step 1, and an object created in step 3
    // waits for the next frame, whatever tick that is
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    AddSpawner();
    ClockSpawner::SpawnTick = 2;

    TickFor(3 * ExactStep);
    EXPECT_TRUE(ClockSpawned::attachTicks.empty()) << "created in step 2, attached next frame";
    TickFor(0.0);
    ASSERT_EQ(ClockSpawned::attachTicks.size(), 1u);
    EXPECT_EQ(ClockSpawned::attachTicks[0], 3u) << "the counter had already reached 3 by then";
}

TEST_F(SimulationClockTest, ANetworkModeDropsNoTimeAndReportsWhenItIsBehind)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application &app = Application::GetInstance();
    app.SetSimulationMode(SimulationMode::Authoritative);
    Clock().SetMaxCatchUpTicks(10);
    AddProbe();

    std::vector<SimulationBehind> reports;
    Clock().SetBehindHandler([&reports](const SimulationBehind &report) { reports.push_back(report); });

    // A frame that owes 64 ticks (one second): ten run, 54 stay owed
    TickFor(64 * ExactStep);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 10u);
    EXPECT_TRUE(Clock().IsBehind());
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].ticksOwed, 54u);
    EXPECT_EQ(reports[0].ticksRunThisFrame, 10u);
    EXPECT_EQ(reports[0].tick, 10u);
    EXPECT_NEAR(reports[0].owedSeconds, 54 * ExactStep, 1e-12);
    EXPECT_NEAR(Clock().GetAccumulator(), 54 * ExactStep, 1e-12);

    // Nothing was dropped: later frames of no time pay the debt, ten ticks at a time, until it is paid
    for (int frame = 0; frame < 5; ++frame)
    {
        TickFor(0.0);
    }
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 60u);
    EXPECT_TRUE(Clock().IsBehind());
    TickFor(0.0);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 64u);
    EXPECT_FALSE(Clock().IsBehind());
    EXPECT_EQ(reports.size(), 6u) << "one report per frame that ended behind";
    EXPECT_EQ(Time::GetTick(), 64u);

    // The ticks ran in order with no gap
    for (std::size_t i = 0; i < ClockProbe::fixedTicks.size(); ++i)
    {
        EXPECT_EQ(ClockProbe::fixedTicks[i], i + 1);
    }
}

TEST_F(SimulationClockTest, AResyncDiscardsTheOwedTimeOnlyWhenTheNetcodeAsksForIt)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application::GetInstance().SetSimulationMode(SimulationMode::Deterministic);
    Clock().SetMaxCatchUpTicks(4);
    AddProbe();

    TickFor(20 * ExactStep);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 4u);
    ASSERT_TRUE(Clock().IsBehind());

    Clock().ResetAccumulator();
    EXPECT_FALSE(Clock().IsBehind());
    TickFor(0.0);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 4u) << "the discarded debt is not run";
}

TEST_F(SimulationClockTest, ANetworkModeWithNoCatchUpLimitRunsEveryTickOwed)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application::GetInstance().SetSimulationMode(SimulationMode::Authoritative);
    Clock().SetMaxCatchUpTicks(0);
    AddProbe();

    TickFor(64 * ExactStep);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 64u);
    EXPECT_FALSE(Clock().IsBehind());

    // Not clamped at 0.25 s either: the whole stall is owed and run
    ClockProbe::Clear();
    TickFor(10.0);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 640u);
}

TEST_F(SimulationClockTest, StepTicksRunsExactlyThatManyTicksWithoutUpdateOrTime)
{
    ASSERT_TRUE(Time::SetFixedTimestep(ExactStep));
    Application &app = Application::GetInstance();
    app.SetSimulationMode(SimulationMode::Deterministic);
    AddProbe();
    TickFor(0.5 * ExactStep);
    ASSERT_EQ(Time::GetTick(), 0u);
    const double accumulator = Clock().GetAccumulator();
    const float timeBefore = Time::GetTime();
    ClockProbe::Clear();

    EXPECT_EQ(app.StepTicks(5), 5u);
    ASSERT_EQ(ClockProbe::fixedTicks.size(), 5u);
    EXPECT_EQ(ClockProbe::fixedTicks.front(), 1u);
    EXPECT_EQ(ClockProbe::fixedTicks.back(), 5u);
    EXPECT_EQ(Time::GetTick(), 5u);
    EXPECT_EQ(ClockProbe::updates, 0) << "headless stepping runs no Update";
    EXPECT_EQ(Time::GetTime(), timeBefore) << "and does not advance the clocks";
    EXPECT_EQ(Clock().GetAccumulator(), accumulator) << "or touch the carried time";
    EXPECT_EQ(app.StepTicks(0), 0u);

    // Paused or not, a network mode steps
    Time::SetTimeScale(0.0f);
    EXPECT_EQ(app.StepTicks(2), 2u);
    EXPECT_EQ(Time::GetTick(), 7u);
}

TEST_F(SimulationClockTest, StepTicksInNoneModeRunsNoTickWhilePausedAndAttachesFirst)
{
    Application &app = Application::GetInstance();
    AddProbe();
    EXPECT_EQ(app.StepTicks(3), 3u);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 3u) << "the probe was attached by the call, as a frame would";

    Time::SetTimeScale(0.0f);
    EXPECT_EQ(app.StepTicks(3), 0u);
    EXPECT_EQ(ClockProbe::fixedTicks.size(), 3u);
}

// ---------------------------------------------------------------------------------------------------------------------
// The clock on its own
// ---------------------------------------------------------------------------------------------------------------------

TEST(SimulationClockUnitTest, NoneModeClampsTheFrameAndNetworkModesKeepIt)
{
    SimulationClock clock;
    clock.BeginFrame(1.0);
    EXPECT_DOUBLE_EQ(clock.GetAccumulator(), SimulationClock::MaxFrameTime);

    clock.Reset();
    clock.SetMode(SimulationMode::Authoritative);
    clock.BeginFrame(1.0);
    EXPECT_DOUBLE_EQ(clock.GetAccumulator(), 1.0);

    clock.SetMode(SimulationMode::None);
    Time::SetTimeScale(1.0f);
}

TEST(SimulationClockUnitTest, ModesAreNamedAndOnlyTheNetworkOnesAreNetworked)
{
    EXPECT_STREQ(ToString(SimulationMode::None), "none");
    EXPECT_STREQ(ToString(SimulationMode::Authoritative), "authoritative");
    EXPECT_STREQ(ToString(SimulationMode::Deterministic), "deterministic");

    SimulationClock clock;
    EXPECT_EQ(clock.GetMode(), SimulationMode::None);
    EXPECT_FALSE(clock.IsNetworked());
    clock.SetMode(SimulationMode::Deterministic);
    EXPECT_TRUE(clock.IsNetworked());
    clock.SetMode(SimulationMode::None);
}

TEST(SimulationClockUnitTest, TicksOwedRoundsDownAndToleratesABadStep)
{
    SimulationClock clock;
    clock.SetMode(SimulationMode::Authoritative);
    clock.BeginFrame(0.5);
    EXPECT_EQ(clock.GetTicksOwed(0.2), 2u);
    EXPECT_EQ(clock.GetTicksOwed(0.0), 0u);
    EXPECT_EQ(clock.GetTicksOwed(-1.0), 0u);
    clock.SetMode(SimulationMode::None);
}
