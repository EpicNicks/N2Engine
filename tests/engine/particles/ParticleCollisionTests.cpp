// Issue #445: per-particle collision of a ParticleSystem with the scene's colliders (docs/particles.html#collision): 2D
// colliders through Box2D, 3D ones through the physics backend, the recorded collisions, layers and the backend it runs
// on. The backend's own rules are in ParticleSimulationTests.cpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/Time.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/rendering/ParticleSystem.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#ifdef N2ENGINE_JOLT_ENABLED
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/jolt/JoltBackend.hpp"
#endif

using namespace N2Engine;
using Rendering::ParticleSystem;
using Math::Vector2;
using Math::Vector3;

namespace
{
    class ParticleCollisionTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;

        void SetUp() override
        {
            Layers::ResetToDefaults();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            Time::SetTimeScale(1.0f);
            ASSERT_TRUE(Time::SetFixedTimestep(0.02));
            SceneManager::AddScene(Scene::Create(std::string("ParticleCollision_") +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
        }

        void TearDown() override
        {
            SceneManager::AddScene(Scene::Create("ParticleCollision_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        GameObject::Ptr Object(const std::string &name, const Vector3 &position)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(position);
            _scene->AddRootGameObject(go);
            return go;
        }

        /// A static 2D box whose top is at y = 0, 20 wide
        GameObject::Ptr Floor2D()
        {
            auto floor = Object("Floor", Vector3(0.0f, -0.5f, 0.0f));
            floor->AddComponent<Physics2D::BoxCollider2D>()->SetHalfExtents(Vector2(10.0f, 0.5f));
            return floor;
        }

        /// A system at y = 2 that fires nothing by itself, with `collide` run before it starts
        template <typename Settings>
        ParticleSystem *Emitter(const Settings &collide)
        {
            auto *system = Object("Sparks", Vector3(0.0f, 2.0f, 0.0f))->AddComponent<ParticleSystem>();
            system->emissionRate = 0.0f;
            system->lifetimeMin = 20.0f;
            system->lifetimeMax = 20.0f;
            system->speedMin = 0.0f;
            system->speedMax = 0.0f;
            system->shapeRadius = 0.0f;
            collide(*system);
            _scene->ProcessAttachQueue();
            return system;
        }

        void Step(const int steps)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                ASSERT_EQ(Application::GetInstance().StepTicks(1), 1u);
                _scene->ProcessDestroyed();
            }
        }

        static float LowestY(const ParticleSystem &system)
        {
            std::vector<Math::Vector4> particles;
            system.GetParticles(particles);
            float lowest = 1000.0f;
            for (const Math::Vector4 &particle : particles)
            {
                lowest = std::min(lowest, particle.y);
            }
            return lowest;
        }
    };
}

#ifdef N2ENGINE_BOX2D_ENABLED

TEST_F(ParticleCollisionTest, ParticlesLandOnA2DColliderAndWithoutCollisionFallThrough)
{
    const auto floor = Floor2D();
    ParticleSystem *colliding = Emitter([](ParticleSystem &system)
    {
        system.collideWith2D = true;
        system.collisionRadius = 0.1f;
        system.bounce = 0.0f;
    });
    ParticleSystem *falling = Emitter([](ParticleSystem &) {});
    EXPECT_EQ(colliding->GetActiveBackendName(), "CPU");
    colliding->EmitBurst(5);
    falling->EmitBurst(5);

    int hits = 0;
    bool sawFloor = false;
    Vector3 normal;
    for (int i = 0; i < 100; ++i)
    {
        Step(1);
        hits += static_cast<int>(colliding->GetCollisionCount());
        for (const ParticleSystem::Collision &collision : colliding->GetCollisions())
        {
            sawFloor = sawFloor || collision.gameObject.lock() == floor;
            normal = collision.normal;
        }
    }
    EXPECT_NEAR(LowestY(*colliding), 0.1f, 0.02f) << "resting its radius above the floor";
    EXPECT_LT(LowestY(*falling), -5.0f);
    EXPECT_GE(hits, 5);
    EXPECT_TRUE(sawFloor);
    EXPECT_NEAR(normal.y, 1.0f, 1e-4f);
    EXPECT_EQ(falling->GetCollisionCount(), 0u);
}

TEST_F(ParticleCollisionTest, LayersAndTriggersAreRespected)
{
    const auto floor = Floor2D();
    floor->SetLayer(Layers::UI);
    ParticleSystem *system = Emitter([](ParticleSystem &s)
    {
        s.collideWith2D = true;
        s.collisionLayers = 1 << Layers::Default;
    });
    system->EmitBurst(1);
    Step(60);
    EXPECT_LT(LowestY(*system), -1.0f) << "the floor's layer is left out";

    floor->SetLayer(Layers::Default);
    floor->GetComponent<Physics2D::BoxCollider2D>()->SetIsTrigger(true);
    system->Clear();
    system->EmitBurst(1);
    Step(60);
    EXPECT_LT(LowestY(*system), -1.0f) << "triggers are left out";

    floor->GetComponent<Physics2D::BoxCollider2D>()->SetIsTrigger(false);
    system->Clear();
    system->EmitBurst(1);
    Step(60);
    EXPECT_GT(LowestY(*system), -0.01f);
}

TEST_F(ParticleCollisionTest, ALifetimeLossOfOneRemovesAParticleAtItsFirstHit)
{
    Floor2D();
    ParticleSystem *system = Emitter([](ParticleSystem &s)
    {
        s.collideWith2D = true;
        s.collisionLifetimeLoss = 1.0f;
    });
    system->EmitBurst(8);
    Step(20);
    EXPECT_EQ(system->GetAliveCount(), 8u);
    Step(60);
    EXPECT_EQ(system->GetAliveCount(), 0u);
}

TEST_F(ParticleCollisionTest, RecordsAreCappedAndOnlyOfTheLastStep)
{
    Floor2D();
    ParticleSystem *system = Emitter([](ParticleSystem &s)
    {
        s.collideWith2D = true;
        s.maxParticles = 200;
    });
    system->EmitBurst(100);
    std::size_t most = 0;
    for (int i = 0; i < 60; ++i)
    {
        Step(1);
        most = std::max(most, system->GetCollisionCount());
        EXPECT_LE(system->GetCollisions().size(), ParticleSystem::kMaxRecordedCollisions);
    }
    EXPECT_EQ(most, 100u) << "all landed in one step";
    system->Clear();
    Step(1);
    EXPECT_EQ(system->GetCollisionCount(), 0u);
    EXPECT_TRUE(system->GetCollisions().empty());
}

#endif

#ifdef N2ENGINE_JOLT_ENABLED

TEST_F(ParticleCollisionTest, ParticlesLandOnA3DCollider)
{
    auto backend = std::make_unique<Physics::JoltBackend>();
    ASSERT_TRUE(backend->Initialize());
    Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
    const auto floor = Object("Floor3D", Vector3(0.0f, -0.5f, 0.0f));
    floor->AddComponent<Physics::BoxCollider>()->SetHalfExtents(Vector3(10.0f, 0.5f, 10.0f));
    ParticleSystem *system = Emitter([](ParticleSystem &s)
    {
        s.collideWith3D = true;
        s.bounce = 0.0f;
    });
    system->EmitBurst(5);
    Step(100);
    EXPECT_NEAR(LowestY(*system), 0.0f, 0.02f);
    // The scene's objects let go of their bodies before the backend does
    SceneManager::AddScene(Scene::Create("ParticleCollision_Jolt_Teardown"), true);
    SceneManager::ProcessAnyPendingSceneChange();
}

#endif
