#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include "SoftBodyTestSupport.hpp"
#include "engine/Application.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/Window.hpp"
#include "engine/config/ApplicationOptions.hpp"
#include "engine/physics/SoftBody.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshRenderer.hpp"
#include "engine/sceneManagement/Scene.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/softbody/SoftBodyBackendSelection.hpp"

// A SoftBody in a headless application (the software renderer, no window or GPU, no physics backend: what the
// editor's play host and CI's Linux runner are): the scene attaches it, Application::Tick steps and draws it, and
// leaving the scene destroys its body. Nothing here needs PhysX or a GPU.

using namespace N2Engine;
using Physics::SoftBody;
using Rendering::Mesh;
using Rendering::MeshRenderer;

namespace
{
    class SoftBodyHeadlessTest : public ::testing::Test
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

            _cpuBefore = SoftBodies::SoftBodyUsage::GetCount(false);
            SceneManager::AddScene(Scene::Create("SoftBodyHeadlessTest_" + std::to_string(++sceneCounter)), true);
            SceneManager::ProcessAnyPendingSceneChange();
            ASSERT_NE(SceneManager::GetCurScene(), nullptr);

            // In front of the default camera (at the origin, looking down -Z)
            _mesh = TestSupport::MakeBoxMesh();
            _object = GameObject::Create("Jelly");
            _object->CreatePositionable();
            _object->GetPositionable()->SetPosition(Math::Vector3(0.0f, 0.0f, -5.0f));
            _renderer = _object->AddComponent<MeshRenderer>();
            _renderer->SetMesh(_mesh);
            SceneManager::GetCurSceneRef().AddRootGameObject(_object);
            _body = _object->AddComponent<SoftBody>();
            _body->resolution = 4;
            _body->stiffness = 1.0e6f;
        }

        void TearDown() override
        {
            // Zero time for whatever runs next, and the scene's components go while the application is still up
            TickOptions rest;
            rest.render = false;
            rest.deltaSeconds = 0.0;
            Application::GetInstance().Tick(rest);
            _body = nullptr;
            _renderer = nullptr;
            _object.reset();
            _mesh.reset();
            SceneManager::AddScene(Scene::Create("SoftBodyHeadlessTest_Empty_" + std::to_string(++sceneCounter)), true);
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

        /// The frame the renderer holds
        static std::vector<std::uint8_t> Frame()
        {
            auto *renderer = Application::GetInstance().GetWindow().GetRenderer();
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Width) * Height * 4);
            renderer->ReadFramebuffer(pixels.data(), Width, Height);
            return pixels;
        }

        static float MeanY(const Mesh &mesh)
        {
            double y = 0.0;
            for (const auto &vertex : mesh.GetVertices())
            {
                y += vertex.position[1];
            }
            return static_cast<float>(y / static_cast<double>(mesh.GetVertices().size()));
        }

        static constexpr int Width = 64;
        static constexpr int Height = 36;
        static inline int sceneCounter = 0;

        std::shared_ptr<Mesh> _mesh;
        GameObject::Ptr _object;
        MeshRenderer *_renderer = nullptr;
        SoftBody *_body = nullptr;
        int _cpuBefore = 0;
    };
}

TEST_F(SoftBodyHeadlessTest, TheSceneStartsTheBodyOnTheCpuBackendAndTheFixedStepDeformsTheRenderMesh)
{
    EXPECT_TRUE(_body->GetActiveBackendName().empty()) << "not attached until the scene processes it";
    EXPECT_EQ(_renderer->GetDeformedMesh(), nullptr);

    TickSteps(1);
    EXPECT_EQ(_body->GetActiveBackendName(), "CPU");
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), _cpuBefore + 1);
    const std::shared_ptr<Mesh> runtime = _body->GetRuntimeMesh();
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(_renderer->GetDeformedMesh(), runtime);

    const std::uint64_t version = runtime->GetVersion();
    const float y = MeanY(*runtime);
    TickSteps(10);
    EXPECT_GT(runtime->GetVersion(), version) << "each fixed step is a new version of the mesh";
    EXPECT_LT(MeanY(*runtime), y - 0.05f) << "the render mesh moved: the body fell";

    // The saved mesh never changed
    EXPECT_EQ(_renderer->GetMesh(), _mesh);
    EXPECT_EQ(_mesh->GetVersion(), 1u);
}

TEST_F(SoftBodyHeadlessTest, ADrawnFrameUsesTheDeformedBodyThroughTheOverride)
{
    TickSteps(1, true);
    const std::shared_ptr<Mesh> runtime = _body->GetRuntimeMesh();
    ASSERT_NE(runtime, nullptr);

    // The renderer drew the runtime copy, not the saved mesh
    const Rendering::GpuCache::Handle &drawn = _renderer->GetDrawResources().GetMeshHandle();
    EXPECT_EQ(drawn.GetSource(), static_cast<const void *>(runtime.get()));

    // Later frames keep drawing it, and it is still the runtime copy
    TickSteps(25, true);
    EXPECT_EQ(_renderer->GetDrawResources().GetMeshHandle().GetSource(), static_cast<const void *>(runtime.get()));
    EXPECT_EQ(_renderer->GetMesh(), _mesh) << "the saved mesh is still the renderer's mesh";
}

TEST_F(SoftBodyHeadlessTest, PausingFreezesTheBodyWhileFramesStillDraw)
{
    TickSteps(10);
    const std::shared_ptr<Mesh> runtime = _body->GetRuntimeMesh();
    ASSERT_NE(runtime, nullptr);
    const std::uint64_t version = runtime->GetVersion();
    const float y = MeanY(*runtime);

    Time::SetTimeScale(0.0f);
    TickSteps(30);
    EXPECT_EQ(runtime->GetVersion(), version) << "no fixed step runs while paused";
    EXPECT_EQ(MeanY(*runtime), y);
    TickSteps(1, true);
    EXPECT_EQ(_renderer->GetDrawResources().GetMeshHandle().GetSource(), static_cast<const void *>(runtime.get()))
        << "the frozen body is still drawn";

    Time::SetTimeScale(1.0f);
    TickSteps(10);
    EXPECT_LT(MeanY(*runtime), y);
}

TEST_F(SoftBodyHeadlessTest, LeavingTheSceneDestroysTheBody)
{
    TickSteps(10);
    ASSERT_NE(_body->GetRuntimeMesh(), nullptr);
    ASSERT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), _cpuBefore + 1);

    // As exiting play mode does: another scene replaces this one, and the body goes with it
    _body = nullptr;
    _renderer = nullptr;
    _object.reset();
    SceneManager::AddScene(Scene::Create("SoftBodyHeadlessTest_Next_" + std::to_string(++sceneCounter)), true);
    SceneManager::ProcessAnyPendingSceneChange();
    EXPECT_EQ(SoftBodies::SoftBodyUsage::GetCount(false), _cpuBefore) << "nothing left running";
}

TEST_F(SoftBodyHeadlessTest, ABodyWithAGroundPlaneStaysAboveItWhileTheSceneRuns)
{
    _body->collideWithGround = true;
    _body->groundHeight = -5.6f; // 5 m below the object (at y = 0), so the body falls about a second before it lands
    TickSteps(300);
    const std::shared_ptr<Mesh> runtime = _body->GetRuntimeMesh();
    ASSERT_NE(runtime, nullptr);
    for (const auto &vertex : runtime->GetVertices())
    {
        // the object sits at y = 0 here, so local y is world y
        EXPECT_GE(vertex.position[1], -5.6f - 1e-3f);
    }
    EXPECT_LT(MeanY(*runtime), -0.5f) << "it fell to the plane";
}
