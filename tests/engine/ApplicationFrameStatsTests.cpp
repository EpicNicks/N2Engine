#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/diagnostics/FrameStats.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// The frame timers in Application::Tick (EN5): one record per Tick with its fixed ticks and sections, nothing while they
// are off, and the same frames either way. A headless software application, as ApplicationTickTests.cpp.

using namespace N2Engine;
using Diagnostics::FrameRecord;
using Diagnostics::FrameStats;

namespace
{
    /// Counts the frame callbacks it gets
    class StatsProbe final : public Component
    {
    public:
        explicit StatsProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "ApplicationFrameStatsTest_Probe"; }

        void OnUpdate() override { ++updates; }
        void OnFixedUpdate() override { ++fixedUpdates; }

        static inline int updates = 0;
        static inline int fixedUpdates = 0;
    };

    class ApplicationFrameStatsTest : public ::testing::Test
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
            Application::GetInstance().Set3DPhysicsBackend(nullptr);

            SceneManager::AddScene(Scene::Create("ApplicationFrameStatsTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);
            const auto object = GameObject::Create("Probe");
            object->CreatePositionable();
            SceneManager::GetCurSceneRef().AddRootGameObject(object);
            object->AddComponent<StatsProbe>();
            StatsProbe::updates = 0;
            StatsProbe::fixedUpdates = 0;
        }

        void TearDown() override
        {
            // The timers belong to the one Application of the process: the next test starts with them off
            Stats().SetEnabled(false);
            TickOptions rest;
            rest.render = false;
            rest.deltaSeconds = 0.0;
            Application::GetInstance().Tick(rest);
            SceneManager::AddScene(Scene::Create("ApplicationFrameStatsTest_Empty_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().GetWindow().Shutdown();
        }

        static FrameStats &Stats() { return Application::GetInstance().GetFrameStats(); }

        static void TickSteps(const int fixedSteps, const bool render = false)
        {
            TickOptions options;
            options.render = render;
            options.deltaSeconds = Time::GetFixedTimestep() * fixedSteps;
            Application::GetInstance().Tick(options);
        }

        static inline int sceneCounter = 0;
    };
}

TEST_F(ApplicationFrameStatsTest, OffByDefaultSoATickRecordsNothing)
{
    ASSERT_FALSE(Stats().IsEnabled());
    TickSteps(1);
    EXPECT_EQ(Stats().Size(), 0u);
}

TEST_F(ApplicationFrameStatsTest, EachTickIsOneRecordWithItsFixedTicksAndSections)
{
    Stats().SetEnabled(true);
    TickSteps(3);
    TickSteps(1);

    const std::vector<FrameRecord> frames = Stats().FramesAfter(0);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[1].frame, frames[0].frame + 1);
    EXPECT_EQ(frames[0].fixedTicks, 3u) << "the three fixed updates of the first Tick";
    EXPECT_EQ(frames[1].fixedTicks, 1u);
    EXPECT_EQ(StatsProbe::fixedUpdates, 4);
    for (const FrameRecord &record : frames)
    {
        EXPECT_GT(record.tickMs, 0.0f);
        EXPECT_GE(record.fixedMs, 0.0f);
        EXPECT_GE(record.updateMs, 0.0f);
        EXPECT_GE(record.audioMs, 0.0f);
        // The sections are parts of the Tick (a little slack for the float rounding of each)
        EXPECT_LE(record.fixedMs + record.updateMs + record.audioMs + record.renderMs, record.tickMs + 0.01f);
        EXPECT_FLOAT_EQ(record.renderMs, 0.0f) << "these Ticks don't draw";
        EXPECT_FLOAT_EQ(record.physicsMs, 0.0f) << "no physics backend";
        EXPECT_FLOAT_EQ(record.commandsMs, 0.0f) << "a host's figure, not the Tick's";
    }
    EXPECT_GT(frames[1].intervalMs, 0.0f) << "the second frame has one before it";
}

TEST_F(ApplicationFrameStatsTest, ADrawingTickTimesItsDrawAndTheRendererCountsTheFrame)
{
    auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
    ASSERT_NE(renderer, nullptr);
    EXPECT_TRUE(renderer->SupportsRenderCounters());
    const std::uint64_t framesBefore = renderer->GetLastFrameCounters().frames;

    Stats().SetEnabled(true);
    TickSteps(1, true);

    const std::vector<FrameRecord> frames = Stats().FramesAfter(0);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_GT(frames[0].renderMs, 0.0f);
    EXPECT_LE(frames[0].renderMs, frames[0].tickMs);
    EXPECT_EQ(renderer->GetLastFrameCounters().frames, framesBefore + 1);
}

TEST_F(ApplicationFrameStatsTest, TheTimersChangeNothingTheGameSees)
{
    // The same frames with the timers off and on: the same callbacks and the same game time
    const float timeBefore = Time::GetTime();
    TickSteps(2);
    TickSteps(1);
    const int fixedOff = StatsProbe::fixedUpdates;
    const int updatesOff = StatsProbe::updates;
    const float timeOff = Time::GetTime() - timeBefore;

    StatsProbe::fixedUpdates = 0;
    StatsProbe::updates = 0;
    Stats().SetEnabled(true);
    const float timeBeforeOn = Time::GetTime();
    TickSteps(2);
    TickSteps(1);

    EXPECT_EQ(StatsProbe::fixedUpdates, fixedOff);
    EXPECT_EQ(StatsProbe::updates, updatesOff);
    EXPECT_NEAR(Time::GetTime() - timeBeforeOn, timeOff, 1e-4f);
    EXPECT_EQ(Stats().Size(), 2u);
}

TEST_F(ApplicationFrameStatsTest, ResetFrameClockGivesTheNextFrameNoInterval)
{
    Stats().SetEnabled(true);
    TickSteps(1);
    Application::GetInstance().ResetFrameClock();
    TickSteps(1);

    const std::vector<FrameRecord> frames = Stats().FramesAfter(0);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_FLOAT_EQ(frames[1].intervalMs, 0.0f);
}
