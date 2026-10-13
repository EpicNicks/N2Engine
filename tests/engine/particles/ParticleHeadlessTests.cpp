#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/common/Color.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/particles/ParticleBackendSelection.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// A ParticleSystem in a headless application (the software renderer, no window or GPU, no physics backend: what the
// editor's play host and CI's Linux runner are): the scene attaches it, Application::Tick steps and draws it, and
// leaving the scene destroys its particles. Nothing here needs PhysX or a GPU.

using namespace N2Engine;
using Rendering::ParticleSystem;

namespace
{
    class ParticleHeadlessTest : public ::testing::Test
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

            // No physics backend, as a host without PhysX: the fixed step is FixedUpdate alone
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));

            _cpuBefore = Particles::ParticleUsage::GetCount(false);
            SceneManager::AddScene(Scene::Create("ParticleHeadlessTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);

            // In front of the default camera (at the origin, looking down -Z)
            _object = GameObject::Create("Fountain");
            _object->CreatePositionable();
            _object->GetPositionable()->SetPosition(Math::Vector3(0.0f, 0.0f, -5.0f));
            SceneManager::GetCurSceneRef().AddRootGameObject(_object);
            _system = _object->AddComponent<ParticleSystem>();
            _system->emissionRate = 100.0f;
            _system->lifetimeMin = 5.0f;
            _system->lifetimeMax = 5.0f;
            _system->speedMin = 0.0f;
            _system->speedMax = 0.0f;
            _system->gravityScale = 0.0f;
            _system->startSize = 1.5f;
            _system->endSize = 1.5f;
            _system->startColor = Common::Color(0.0f, 1.0f, 0.0f, 1.0f);
            _system->endColor = Common::Color(0.0f, 1.0f, 0.0f, 1.0f);
        }

        void TearDown() override
        {
            // Zero time for whatever runs next, and the scene's components go while the application is still up
            TickOptions rest;
            rest.render = false;
            rest.deltaSeconds = 0.0;
            Application::GetInstance().Tick(rest);
            _system = nullptr;
            _object.reset();
            SceneManager::AddScene(Scene::Create("ParticleHeadlessTest_Empty_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().GetWindow().Shutdown();
        }

        static void TickSteps(const int fixedSteps, const bool render = false)
        {
            TickOptions options;
            options.render = render;
            options.deltaSeconds = Time::GetFixedTimestep() * fixedSteps;
            Application::GetInstance().Tick(options);
        }

        /// How many pixels of the frame the renderer holds are the particles' green
        static int GreenPixels()
        {
            auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Width) * Height * 4);
            renderer->ReadFramebuffer(pixels.data(), Width, Height);
            int green = 0;
            for (std::size_t i = 0; i < pixels.size(); i += 4)
            {
                if (pixels[i] == 0 && pixels[i + 1] == 255 && pixels[i + 2] == 0)
                {
                    ++green;
                }
            }
            return green;
        }

        static constexpr int Width = 64;
        static constexpr int Height = 36;
        static inline int sceneCounter = 0;

        GameObject::Ptr _object;
        ParticleSystem *_system = nullptr;
        int _cpuBefore = 0;
    };
}

TEST_F(ParticleHeadlessTest, TheSceneStartsTheSystemOnTheCpuBackendAndTheFixedStepEmits)
{
    EXPECT_TRUE(_system->GetActiveBackendName().empty()) << "not attached until the scene processes it";
    TickSteps(1);
    EXPECT_EQ(_system->GetActiveBackendName(), "CPU");
    EXPECT_TRUE(_system->IsPlaying());
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), _cpuBefore + 1);

    for (int i = 0; i < 24; ++i)
    {
        TickSteps(1);
    }
    // 25 fixed steps at 100 a second and 0.02 s a step: two a step
    EXPECT_NEAR(static_cast<double>(_system->GetAliveCount()), 50.0, 3.0);
}

TEST_F(ParticleHeadlessTest, ADrawnFrameShowsTheParticles)
{
    TickSteps(10);
    ASSERT_GT(_system->GetAliveCount(), 0u);
    TickSteps(1, true);
    EXPECT_GT(GreenPixels(), 0) << "the software renderer drew the billboards";
}

TEST_F(ParticleHeadlessTest, PausingFreezesTheSystemWhileFramesStillDraw)
{
    TickSteps(10);
    const std::size_t before = _system->GetAliveCount();
    ASSERT_GT(before, 0u);

    Time::SetTimeScale(0.0f);
    TickSteps(30);
    EXPECT_EQ(_system->GetAliveCount(), before) << "no fixed step runs while paused";
    TickSteps(1, true);
    EXPECT_GT(GreenPixels(), 0) << "the frozen particles are still drawn";

    Time::SetTimeScale(1.0f);
    TickSteps(10);
    EXPECT_GT(_system->GetAliveCount(), before);
}

TEST_F(ParticleHeadlessTest, LeavingTheSceneDestroysTheParticles)
{
    TickSteps(10);
    ASSERT_GT(_system->GetAliveCount(), 0u);
    ASSERT_EQ(Particles::ParticleUsage::GetCount(false), _cpuBefore + 1);

    // As exiting play mode does: another scene replaces this one, and the particles go with it
    _system = nullptr;
    _object.reset();
    SceneManager::AddScene(Scene::Create("ParticleHeadlessTest_Next_" + std::to_string(++sceneCounter)), true);
    SceneManager::ProcessAnyPendingSceneChange();
    EXPECT_EQ(Particles::ParticleUsage::GetCount(false), _cpuBefore) << "nothing left running";
}
