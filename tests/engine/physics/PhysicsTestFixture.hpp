#pragma once

// The scene fixture of the physics conformance tests that run on every backend (PhysicsCoreTests.cpp) and of the
// Jolt-only ones (JoltBackendTests.cpp). Application::Init needs a window, so the backend is installed directly and
// frames are stepped the way Application::PhysicsUpdate does.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <type_traits>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/jolt/JoltBackend.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

#if defined(N2ENGINE_PHYSX_ENABLED) || defined(N2ENGINE_JOLT_ENABLED)
#define N2_PHYSICS_TEST_BACKENDS 1

namespace N2PhysicsTests
{
    using namespace N2Engine;
    using namespace N2Engine::Physics;
    using Math::Vector3;

#ifdef N2ENGINE_PHYSX_ENABLED
    struct PhysXTag
    {
        using Backend = PhysXBackend;
        static constexpr const char *kName = "PhysX";
        static constexpr bool kIsJolt = false;
    };
#endif

#ifdef N2ENGINE_JOLT_ENABLED
    struct JoltTag
    {
        using Backend = JoltBackend;
        static constexpr const char *kName = "Jolt";
        static constexpr bool kIsJolt = true;
    };
#endif

    // The backends the typed tests run on: every one this build has
    using AllBackends = ::testing::Types<
#if defined(N2ENGINE_PHYSX_ENABLED) && defined(N2ENGINE_JOLT_ENABLED)
        PhysXTag, JoltTag
#elif defined(N2ENGINE_PHYSX_ENABLED)
        PhysXTag
#else
        JoltTag
#endif
        >;

    // Names the typed suites' instances "PhysX" and "Jolt" in the test output
    struct BackendNames
    {
        template <typename Tag>
        static std::string GetName(int)
        {
            return Tag::kName;
        }
    };

    inline void ExpectVectorNear(const Vector3 &actual, const Vector3 &expected, const float tolerance)
    {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

    // A headless scene stepped on a real backend of type Tag::Backend, installed once for the suite
    template <typename Tag>
    class PhysicsCoreFixture : public ::testing::Test
    {
    protected:
        using Backend = typename Tag::Backend;

        static void SetUpTestSuite()
        {
            auto backend = std::make_unique<Backend>();
            ASSERT_TRUE(backend->Initialize());
            Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
        }

        static void TearDownTestSuite()
        {
            // Leave no loaded scene holding bodies of a backend that's about to go away
            SceneManager::AddScene(Scene::Create("PhysicsCore_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        Scene *_scene = nullptr;

        void SetUp() override
        {
            SceneManager::AddScene(Scene::Create(std::string("PhysicsCore_") +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name()),
                                   true);
            SceneManager::ProcessAnyPendingSceneChange(); // tears down the previous test's bodies
            _scene = SceneManager::GetCurScene();
        }

        void TearDown() override
        {
            // A test that changed gravity must not leak it into the next one (the backend lives for the whole suite)
            Native().SetGravity(Vector3(0.0f, -9.81f, 0.0f));
            Native().ApplyPendingChanges();
        }

        static IPhysicsBackend &BackendRef() { return *Application::GetInstance().Get3DPhysicsBackend(); }
        // The backend as its own type, for the diagnostics both have (GetInertiaTensor, GetCenterOfMass, GetShapeCount)
        static Backend &Native() { return static_cast<Backend &>(BackendRef()); }

        void Step(const int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                BackendRef().ApplyPendingChanges();
                _scene->FixedUpdate();
                BackendRef().Update(1.0f / 60.0f);
                BackendRef().SyncTransforms();
                BackendRef().ProcessCollisionCallbacks();
                _scene->ProcessDestroyed();
            }
        }

        GameObject::Ptr Spawn(const std::string &name, const Vector3 &position)
        {
            const auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(position);
            _scene->AddRootGameObject(go);
            return go;
        }

        // Static 10x1x10 floor whose top surface is at y = 0.5
        GameObject::Ptr SpawnFloor()
        {
            const auto floor = Spawn("Floor", Vector3(0.0f, 0.0f, 0.0f));
            floor->AddComponent<BoxCollider>()->SetSize(Vector3(10.0f, 1.0f, 10.0f));
            return floor;
        }

        GameObject::Ptr SpawnBall(const std::string &name, const Vector3 &position)
        {
            const auto ball = Spawn(name, position);
            ball->AddComponent<SphereCollider>()->SetRadius(0.5f);
            auto *body = ball->AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(true);
            return ball;
        }

        // Dynamic and weightless (so it stays put). The body is only created when the attach queue runs in the next
        // Step, so the mass here is the one it is created with.
        Rigidbody *AddFloatingBody(GameObject &go, const float mass)
        {
            auto *body = go.AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(false);
            body->SetMass(mass);
            return body;
        }
    };
}

#endif // a physics backend is in this build
