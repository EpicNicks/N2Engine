#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/common/Color.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// Application::Tick (#82, E9): the body of Run()'s loop, extracted so a host with a loop of its own (the editor's play
// child) can run game frames between the requests of its client. A headless software application: no display or GPU.

using namespace N2Engine;

namespace
{
    /// Counts the frame callbacks it gets
    class TickProbe final : public Component
    {
    public:
        explicit TickProbe(GameObject &gameObject) : Component(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "ApplicationTickTest_Probe"; }

        void OnUpdate() override { ++updates; }
        void OnFixedUpdate() override { ++fixedUpdates; }
        void OnLateUpdate() override { ++lateUpdates; }

        static inline int updates = 0;
        static inline int fixedUpdates = 0;
        static inline int lateUpdates = 0;
    };

    class ApplicationTickTest : public ::testing::Test
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
            ASSERT_TRUE(window.SetRenderSize(Width, Height));

            // No physics backend: the fixed step is FixedUpdate alone
            Application::GetInstance().Set3DPhysicsBackend(nullptr);

            // A name of its own each time: the scenes the manager stores are kept by name
            SceneManager::AddScene(Scene::Create("ApplicationTickTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);

            const auto object = GameObject::Create("Probe");
            object->CreatePositionable();
            SceneManager::GetCurSceneRef().AddRootGameObject(object);
            object->AddComponent<TickProbe>();

            TickProbe::updates = 0;
            TickProbe::fixedUpdates = 0;
            TickProbe::lateUpdates = 0;
        }

        void TearDown() override
        {
            // Other tests of this program rely on the engine clock standing still (nothing else ticks it): a frame of no
            // time leaves the frame deltas at zero again
            TickOptions rest;
            rest.render = false;
            rest.deltaSeconds = 0.0;
            Application::GetInstance().Tick(rest);

            // The scene's components go while the application is still up
            SceneManager::AddScene(Scene::Create("ApplicationTickTest_Empty_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().GetWindow().Shutdown();
        }

        /// One Tick of exactly `fixedSteps` fixed timesteps of time, with no drawing
        static void TickSteps(const int fixedSteps)
        {
            TickOptions options;
            options.render = false;
            options.deltaSeconds = Time::GetFixedTimestep() * fixedSteps;
            Application::GetInstance().Tick(options);
        }

        /// The red channel of the first pixel of the frame the renderer holds
        static int FirstPixelRed()
        {
            auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Width) * Height * 4);
            renderer->ReadFramebuffer(pixels.data(), Width, Height);
            return pixels[0];
        }

        static constexpr int Width = 8;
        static constexpr int Height = 4;
        static inline int sceneCounter = 0;
    };
}

TEST_F(ApplicationTickTest, AFrameOfOneFixedTimestepRunsOneFixedUpdateAndOneUpdate)
{
    TickSteps(1);

    EXPECT_EQ(TickProbe::fixedUpdates, 1);
    EXPECT_EQ(TickProbe::updates, 1);
    EXPECT_EQ(TickProbe::lateUpdates, 1);
}

TEST_F(ApplicationTickTest, AFrameOfSeveralTimestepsCatchesUpWithFixedUpdatesButRunsOneUpdate)
{
    TickSteps(3);

    EXPECT_EQ(TickProbe::fixedUpdates, 3);
    EXPECT_EQ(TickProbe::updates, 1);
}

TEST_F(ApplicationTickTest, TimeAdvancesByTheDeltaGivenNotByTheClock)
{
    const float timeBefore = Time::GetTime();
    const float unscaledBefore = Time::GetUnscaledTime();
    const double step = Time::GetFixedTimestep();

    // Whatever the real time between the calls, a frame of a given delta is that long
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    TickSteps(2);

    EXPECT_NEAR(Time::GetUnscaledDeltaTime(), static_cast<float>(2 * step), 1e-6f);
    EXPECT_NEAR(Time::GetDeltaTime(), static_cast<float>(2 * step) * Time::GetTimeScale(), 1e-6f);
    EXPECT_NEAR(Time::GetUnscaledTime() - unscaledBefore, static_cast<float>(2 * step), 1e-4f);
    EXPECT_NEAR(Time::GetTime() - timeBefore, static_cast<float>(2 * step) * Time::GetTimeScale(), 1e-4f);
}

TEST_F(ApplicationTickTest, WithoutADeltaAFrameIsMeasuredByTheClock)
{
    Application::GetInstance().ResetFrameClock();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    TickOptions options;
    options.render = false;
    Application::GetInstance().Tick(options);

    EXPECT_GE(Time::GetUnscaledDeltaTime(), 0.05f);
    // The next frame is caught up on by fixed updates at most MaxFrameTime (0.25 s) deep, so this one ran some
    EXPECT_GE(TickProbe::fixedUpdates, 1);
    EXPECT_EQ(TickProbe::updates, 1);

    // This frame left time in the fixed-step accumulator (less than one step: whole steps were run). That remainder carries
    // to the next test's first frame, which still runs exactly the steps its own delta pays for, since a remainder under
    // one step plus k steps is k steps.
}

TEST_F(ApplicationTickTest, ResetFrameClockMakesAPausedStretchNotAFrame)
{
    Application::GetInstance().ResetFrameClock();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Resuming after a pause: the time since the last frame is dropped
    Application::GetInstance().ResetFrameClock();

    TickOptions options;
    options.render = false;
    Application::GetInstance().Tick(options);

    EXPECT_LT(Time::GetUnscaledDeltaTime(), 0.1f);
}

TEST_F(ApplicationTickTest, ATickWithRenderOffDrawsNothingAndRenderGameFrameDrawsWithoutRunningAFrame)
{
    Window &window = Application::GetInstance().GetWindow();

    window.clearColor = Common::Color::Red;
    Application::GetInstance().RenderGameFrame();
    ASSERT_EQ(FirstPixelRed(), 255);
    EXPECT_EQ(TickProbe::updates, 0) << "drawing isn't running a frame";
    EXPECT_EQ(TickProbe::fixedUpdates, 0);

    // Another clear colour: a Tick that doesn't render leaves the picture it had
    window.clearColor = Common::Color::Black;
    TickSteps(1);
    EXPECT_EQ(FirstPixelRed(), 255) << "the Tick with render off drew";

    // The default Tick draws, as Run() does
    TickOptions options;
    options.deltaSeconds = Time::GetFixedTimestep();
    Application::GetInstance().Tick(options);
    EXPECT_EQ(FirstPixelRed(), 0);
}

TEST_F(ApplicationTickTest, RenderGameFrameLeavesTheGameTimeAlone)
{
    TickSteps(1);
    const float time = Time::GetTime();
    const float delta = Time::GetUnscaledDeltaTime();

    Application::GetInstance().RenderGameFrame();
    Application::GetInstance().RenderGameFrame();

    EXPECT_EQ(Time::GetTime(), time);
    EXPECT_EQ(Time::GetUnscaledDeltaTime(), delta);
}
