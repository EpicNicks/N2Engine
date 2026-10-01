#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// Layers in a real PhysX scene: query masks, the collision matrix (contacts and triggers) and runtime changes
#ifdef N2ENGINE_PHYSX_ENABLED

using namespace N2Engine;
using namespace N2Engine::Physics;
using Math::Vector3;

namespace
{
    constexpr int LayerA = 8;
    constexpr int LayerB = 9;
    constexpr int LayerC = 10;

    class LayerEventCounter final : public Component
    {
    public:
        explicit LayerEventCounter(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "LayerEventCounter"; }

        void OnCollisionEnter(const Collision &) override { ++collisionEnter; }
        void OnCollisionStay(const Collision &) override { ++collisionStay; }
        void OnCollisionExit(const Collision &) override { ++collisionExit; }
        void OnTriggerEnter(const Trigger) override { ++triggerEnter; }
        void OnTriggerStay(const Trigger) override { ++triggerStay; }
        void OnTriggerExit(const Trigger) override { ++triggerExit; }

        int collisionEnter = 0, collisionStay = 0, collisionExit = 0;
        int triggerEnter = 0, triggerStay = 0, triggerExit = 0;
    };

    const Vector3 Downward(0.0f, -1.0f, 0.0f);
    const Vector3 AlongZ(0.0f, 0.0f, 1.0f);
}

// Like PhysicsTest: the backend is installed directly and frames are stepped as Application::PhysicsUpdate does
class LayerPhysicsTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        auto backend = std::make_unique<PhysXBackend>();
        ASSERT_TRUE(backend->Initialize());
        Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
    }

    static void TearDownTestSuite()
    {
        SceneManager::AddScene(Scene::Create("LayerPhysicsTest_Teardown"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
    }

    Scene *_scene = nullptr;

    void SetUp() override
    {
        Layers::ResetToDefaults();
        SceneManager::AddScene(Scene::Create(std::string("LayerPhysics_") +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
    }

    void TearDown() override { Layers::ResetToDefaults(); }

    static IPhysicsBackend &Backend() { return *Application::GetInstance().Get3DPhysicsBackend(); }

    void Step(const int steps = 1)
    {
        for (int i = 0; i < steps; ++i)
        {
            _scene->ProcessAttachQueue();
            Backend().ApplyPendingChanges();
            _scene->FixedUpdate();
            Backend().Update(1.0f / 60.0f);
            Backend().SyncTransforms();
            Backend().ProcessCollisionCallbacks();
            _scene->ProcessDestroyed();
        }
    }

    GameObject::Ptr Spawn(const std::string &name, const Vector3 &position, const int layer)
    {
        const auto go = GameObject::Create(name);
        go->CreatePositionable();
        go->GetPositionable()->SetPosition(position);
        go->SetLayer(layer);
        _scene->AddRootGameObject(go);
        return go;
    }

    GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const int layer)
    {
        const auto go = Spawn(name, position, layer);
        go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        return go;
    }

    // Static 10x1x10 floor whose top surface is at y = 0.5
    GameObject::Ptr SpawnFloor(const int layer)
    {
        const auto floor = Spawn("Floor", Vector3(0.0f, 0.0f, 0.0f), layer);
        floor->AddComponent<BoxCollider>()->SetSize(Vector3(10.0f, 1.0f, 10.0f));
        return floor;
    }

    GameObject::Ptr SpawnBall(const std::string &name, const Vector3 &position, const int layer)
    {
        const auto ball = Spawn(name, position, layer);
        ball->AddComponent<SphereCollider>()->SetRadius(0.5f);
        auto *body = ball->AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        body->SetGravityEnabled(true);
        return ball;
    }

    // A 4x4x4 trigger volume at the origin
    GameObject::Ptr SpawnZone(const int layer)
    {
        const auto zone = Spawn("Zone", Vector3(0.0f, 0.0f, 0.0f), layer);
        auto *collider = zone->AddComponent<BoxCollider>();
        collider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
        collider->SetIsTrigger(true);
        return zone;
    }

    // A weightless dynamic sphere inside the zone (it stays put and keeps overlapping)
    GameObject::Ptr SpawnVisitor(const int layer)
    {
        const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f), layer);
        visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
        auto *body = visitor->AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        body->SetGravityEnabled(false);
        return visitor;
    }
};

// ============================================================================
// Queries
// ============================================================================

TEST_F(LayerPhysicsTest, QueryMaskIncludesAndExcludesLayers)
{
    const auto nearBox = SpawnBox("Near", Vector3(0.0f, 0.0f, 5.0f), LayerA);
    const auto farBox = SpawnBox("Far", Vector3(0.0f, 0.0f, 10.0f), LayerB);
    Step(1);

    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(Vector3::Zero, AlongZ, hit, 100.0f, Layers::MaskOf(LayerB)));
    EXPECT_EQ(hit.gameObject, farBox.get()) << "the near box isn't on the mask's layer";
    ASSERT_TRUE(Raycast::Single(Vector3::Zero, AlongZ, hit, 100.0f, Layers::MaskOf(LayerA)));
    EXPECT_EQ(hit.gameObject, nearBox.get());

    std::vector<RaycastHit> hits;
    EXPECT_EQ(Raycast::All(Vector3::Zero, AlongZ, hits, 100.0f, Layers::MaskOf(LayerA) | Layers::MaskOf(LayerB)), 2);
    EXPECT_EQ(Raycast::All(Vector3::Zero, AlongZ, hits, 100.0f, Layers::MaskOf(LayerB)), 1);
    EXPECT_EQ(Raycast::All(Vector3::Zero, AlongZ, hits, 100.0f, Layers::MaskOf(LayerC)), 0);

    ASSERT_TRUE(Raycast::SphereCast(Vector3::Zero, 0.1f, AlongZ, hit, 100.0f, Layers::MaskOf(LayerB)));
    EXPECT_EQ(hit.gameObject, farBox.get());
    EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(LayerC)));
}

TEST_F(LayerPhysicsTest, IgnoreRaycastIsSkippedByDefault)
{
    const auto ignored = SpawnBox("Ignored", Vector3(0.0f, 0.0f, 5.0f), Layers::IgnoreRaycast);
    const auto target = SpawnBox("Target", Vector3(0.0f, 0.0f, 10.0f), Layers::Default);
    Step(1);

    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(Vector3::Zero, AlongZ, hit, 100.0f));
    EXPECT_EQ(hit.gameObject, target.get()) << "the default mask hit the Ignore Raycast layer";
    std::vector<RaycastHit> hits;
    EXPECT_EQ(Raycast::All(Vector3::Zero, AlongZ, hits, 100.0f), 1);
    ASSERT_TRUE(Raycast::SphereCast(Vector3::Zero, 0.1f, AlongZ, hit, 100.0f));
    EXPECT_EQ(hit.gameObject, target.get());

    // Asked for explicitly, it's hit like any other layer
    ASSERT_TRUE(Raycast::Single(Vector3::Zero, AlongZ, hit, 100.0f, Layers::AllLayers));
    EXPECT_EQ(hit.gameObject, ignored.get());
}

TEST_F(LayerPhysicsTest, MaskZeroHitsNothing)
{
    SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f), Layers::Default);
    Step(1);

    // An all-zero mask used to switch PhysX's layer test off and hit everything
    RaycastHit hit;
    EXPECT_FALSE(Raycast::Single(Vector3::Zero, AlongZ, hit, 100.0f, 0u));
    EXPECT_FALSE(hit.hit);
    std::vector<RaycastHit> hits;
    EXPECT_EQ(Raycast::All(Vector3::Zero, AlongZ, hits, 100.0f, 0u), 0);
    EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.1f, AlongZ, hit, 100.0f, 0u));
    EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, 0u));
}

TEST_F(LayerPhysicsTest, ChangingAnObjectsLayerMovesItsQueryLayerAtOnce)
{
    const auto box = SpawnBox("Box", Vector3(0.0f, 0.0f, 5.0f), Layers::Default);
    Step(1);
    ASSERT_TRUE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f));

    box->SetLayer(Layers::IgnoreRaycast);
    EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f));
    EXPECT_TRUE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(Layers::IgnoreRaycast)));

    box->SetLayer(LayerA);
    EXPECT_TRUE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(LayerA)));
    EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(Layers::IgnoreRaycast)));
}

TEST_F(LayerPhysicsTest, ChildColliderUsesItsOwnLayer)
{
    const auto parent = GameObject::Create("Parent");
    parent->CreatePositionable();
    parent->SetLayer(LayerA);
    parent->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    parent->AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);

    const auto child = GameObject::Create("Child");
    child->CreatePositionable();
    parent->AddChild(child);
    child->GetPositionable()->SetPosition(Vector3(5.0f, 0.0f, 0.0f));
    child->SetLayer(LayerB);
    child->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));

    _scene->AddRootGameObject(parent);
    Step(1);

    const Vector3 aboveChild(5.0f, 5.0f, 0.0f);
    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(aboveChild, Downward, hit, 100.0f, Layers::MaskOf(LayerB)));
    EXPECT_EQ(hit.gameObject, child.get());
    EXPECT_FALSE(Raycast::Any(aboveChild, Downward, 100.0f, Layers::MaskOf(LayerA))) << "the child took its parent's layer";
    EXPECT_TRUE(Raycast::Any(Vector3(0.0f, 5.0f, 0.0f), Downward, 100.0f, Layers::MaskOf(LayerA)));

    parent->SetLayerRecursive(LayerC);
    EXPECT_TRUE(Raycast::Any(aboveChild, Downward, 100.0f, Layers::MaskOf(LayerC)));
    EXPECT_FALSE(Raycast::Any(aboveChild, Downward, 100.0f, Layers::MaskOf(LayerB)));
}

// ============================================================================
// Collision matrix
// ============================================================================

TEST_F(LayerPhysicsTest, NonCollidingLayersPassThroughEachOther)
{
    Layers::SetCollision(LayerA, LayerB, false);
    const auto floor = SpawnFloor(LayerA);
    auto *floorEvents = floor->AddComponent<LayerEventCounter>();
    const auto ball = SpawnBall("Ghost", Vector3(0.0f, 2.0f, 0.0f), LayerB);
    auto *ballEvents = ball->AddComponent<LayerEventCounter>();

    Step(90);

    EXPECT_LT(ball->GetPositionable()->GetPosition().y, -1.0f) << "the ball landed on a floor it shouldn't touch";
    EXPECT_EQ(ballEvents->collisionEnter, 0);
    EXPECT_EQ(floorEvents->collisionEnter, 0);
}

TEST_F(LayerPhysicsTest, TurningCollisionOffMidContactEndsThePair)
{
    const auto floor = SpawnFloor(LayerA);
    auto *floorEvents = floor->AddComponent<LayerEventCounter>();
    const auto ball = SpawnBall("Lander", Vector3(0.0f, 2.0f, 0.0f), LayerB);
    auto *ballEvents = ball->AddComponent<LayerEventCounter>();

    Step(180); // lands and comes to rest
    ASSERT_GT(ballEvents->collisionEnter, ballEvents->collisionExit) << "the ball should be resting on the floor";

    Layers::SetCollision(LayerA, LayerB, false);
    Step(1);
    EXPECT_EQ(ballEvents->collisionExit, ballEvents->collisionEnter) << "the suppressed pair never exited";
    EXPECT_EQ(floorEvents->collisionExit, floorEvents->collisionEnter);
    const int stays = ballEvents->collisionStay;

    Step(60);
    EXPECT_EQ(ballEvents->collisionStay, stays) << "the suppressed pair still stays";
    EXPECT_LT(ball->GetPositionable()->GetPosition().y, 0.0f) << "the ball should have fallen through";
}

TEST_F(LayerPhysicsTest, TriggersObeyTheMatrix)
{
    Layers::SetCollision(LayerA, LayerB, false);
    const auto zone = SpawnZone(LayerA);
    auto *zoneEvents = zone->AddComponent<LayerEventCounter>();
    const auto visitor = SpawnVisitor(LayerB);
    auto *visitorEvents = visitor->AddComponent<LayerEventCounter>();

    Step(5);

    EXPECT_EQ(zoneEvents->triggerEnter, 0) << "a trigger fired between layers that don't collide";
    EXPECT_EQ(visitorEvents->triggerEnter, 0);
}

TEST_F(LayerPhysicsTest, MatrixChangesEndAndRestartOverlappingTriggers)
{
    const auto zone = SpawnZone(LayerA);
    auto *zoneEvents = zone->AddComponent<LayerEventCounter>();
    const auto visitor = SpawnVisitor(LayerB);
    auto *visitorEvents = visitor->AddComponent<LayerEventCounter>();

    Step(5);
    ASSERT_EQ(zoneEvents->triggerEnter, 1);
    ASSERT_EQ(visitorEvents->triggerEnter, 1);

    Layers::SetCollision(LayerA, LayerB, false);
    Step(1);
    const int stays = zoneEvents->triggerStay;
    Step(5);

    EXPECT_EQ(zoneEvents->triggerExit, 1);
    EXPECT_EQ(visitorEvents->triggerExit, 1);
    EXPECT_EQ(zoneEvents->triggerStay, stays) << "the suppressed pair still stays";

    // Still overlapping, so allowing it again starts it again
    Layers::SetCollision(LayerA, LayerB, true);
    Step(5);

    EXPECT_EQ(zoneEvents->triggerEnter, 2);
    EXPECT_EQ(visitorEvents->triggerEnter, 2);
    EXPECT_EQ(zoneEvents->triggerExit, 1);
}

TEST_F(LayerPhysicsTest, ChangingAnObjectsLayerAtRuntimeRefilters)
{
    Layers::SetCollision(LayerA, LayerB, false);
    const auto zone = SpawnZone(LayerA);
    auto *zoneEvents = zone->AddComponent<LayerEventCounter>();
    const auto visitor = SpawnVisitor(LayerC);
    auto *visitorEvents = visitor->AddComponent<LayerEventCounter>();

    Step(5);
    ASSERT_EQ(visitorEvents->triggerEnter, 1);

    // To a layer the zone doesn't collide with: the pair ends
    visitor->SetLayer(LayerB);
    Step(5);
    EXPECT_EQ(visitorEvents->triggerExit, 1);
    EXPECT_EQ(zoneEvents->triggerExit, 1);

    // Back to one it does: it starts again
    visitor->SetLayer(LayerC);
    Step(5);
    EXPECT_EQ(visitorEvents->triggerEnter, 2);
    EXPECT_EQ(zoneEvents->triggerEnter, 2);

    // To another layer that still collides: the pair carries on, with no Exit and Enter in between
    const int stays = zoneEvents->triggerStay;
    visitor->SetLayer(Layers::Default);
    Step(5);
    EXPECT_EQ(visitorEvents->triggerExit, 1);
    EXPECT_EQ(visitorEvents->triggerEnter, 2);
    EXPECT_EQ(zoneEvents->triggerExit, 1);
    EXPECT_EQ(zoneEvents->triggerEnter, 2);
    EXPECT_GT(zoneEvents->triggerStay, stays);
}

TEST_F(LayerPhysicsTest, CollidersAttachedAfterAMatrixChangeUseTheNewMatrix)
{
    const auto floor = SpawnFloor(LayerA);
    Step(1);

    Layers::SetCollision(LayerA, LayerB, false);
    const auto ball = SpawnBall("Late", Vector3(0.0f, 2.0f, 0.0f), LayerB);
    auto *ballEvents = ball->AddComponent<LayerEventCounter>();
    Step(90);

    EXPECT_EQ(ballEvents->collisionEnter, 0);
    EXPECT_LT(ball->GetPositionable()->GetPosition().y, -1.0f);
}

#endif
