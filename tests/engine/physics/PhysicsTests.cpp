#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Component.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics/BoxCollider.hpp"
#include "engine/physics/CapsuleCollider.hpp"
#include "engine/physics/Raycast.hpp"
#include "engine/physics/Rigidbody.hpp"
#include "engine/physics/SphereCollider.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"
#include "engine/sceneManagement/SceneManager.hpp"

// These drive a real PhysX scene; a build without PhysX (N2ENGINE_USE_PHYSX=OFF) has nothing to test here
#ifdef N2ENGINE_PHYSX_ENABLED

using namespace N2Engine;
using namespace N2Engine::Physics;
using Math::Vector3;

namespace
{
    // Records physics events delivered to its GameObject
    class EventRecorder final : public Component
    {
    public:
        explicit EventRecorder(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "EventRecorder"; }

        void OnCollisionEnter(const Collision &c) override
        {
            if (collisionEnter == 0)
            {
                firstEnterImpulse = c.impulse;
                firstEnterRelativeVelocity = c.relativeVelocity;
            }
            ++collisionEnter;
            lastCollisionSelf = c.gameObject;
            lastCollisionOther = c.otherGameObject;
            lastCollisionCollider = c.collider;
            lastCollisionOtherCollider = c.otherCollider;
            if (onCollisionEnter)
            {
                onCollisionEnter();
            }
        }
        void OnCollisionStay(const Collision &) override { ++collisionStay; }
        void OnCollisionExit(const Collision &) override { ++collisionExit; }
        void OnTriggerEnter(const Trigger t) override
        {
            ++triggerEnter;
            lastTriggerSelf = t.gameObject;
            lastTriggerOther = t.otherGameObject;
            if (!firstTriggerCollider)
            {
                firstTriggerCollider = t.collider;
                firstTriggerOtherCollider = t.otherCollider;
            }
        }
        void OnTriggerStay(const Trigger) override { ++triggerStay; }
        void OnTriggerExit(const Trigger t) override
        {
            ++triggerExit;
            lastTriggerExitSelf = t.gameObject;
        }

        [[nodiscard]] int Total() const
        {
            return collisionEnter + collisionStay + collisionExit + triggerEnter + triggerStay + triggerExit;
        }

        int collisionEnter = 0, collisionStay = 0, collisionExit = 0;
        int triggerEnter = 0, triggerStay = 0, triggerExit = 0;
        GameObject *lastCollisionSelf = nullptr, *lastCollisionOther = nullptr;
        GameObject *lastTriggerSelf = nullptr, *lastTriggerOther = nullptr;
        ICollider *lastCollisionCollider = nullptr, *lastCollisionOtherCollider = nullptr;
        ICollider *firstTriggerCollider = nullptr, *firstTriggerOtherCollider = nullptr;
        GameObject *lastTriggerExitSelf = nullptr;
        Vector3 firstEnterImpulse = Vector3::Zero, firstEnterRelativeVelocity = Vector3::Zero;
        std::function<void()> onCollisionEnter;
    };

    int ShapesUnder(const Vector3 &above)
    {
        std::vector<RaycastHit> hits;
        return Raycast::All(above, Vector3(0.0f, -1.0f, 0.0f), hits, 100.0f);
    }

    void ExpectNear(const Vector3 &actual, const Vector3 &expected, const float tolerance = 1e-4f)
    {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }
}

// Runs a real PhysX scene headless. Application::Init needs a window, so the backend is installed
// directly, and frames are stepped the way Application::PhysicsUpdate does.
class PhysicsTest : public ::testing::Test
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
        // Leave no loaded scene holding bodies of a backend that's about to go away
        SceneManager::AddScene(Scene::Create("PhysicsTest_Teardown"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        Application::GetInstance().Set3DPhysicsBackend(nullptr);
    }

    Scene *_scene = nullptr;

    void SetUp() override
    {
        SceneManager::AddScene(Scene::Create(std::string("Physics_") +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
        SceneManager::ProcessAnyPendingSceneChange(); // tears down the previous test's bodies
        _scene = SceneManager::GetCurScene();
    }

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
};

// ============================================================================
// Collision and trigger events
// ============================================================================

TEST_F(PhysicsTest, CollisionEventsFireOnBothBodies)
{
    const auto floor = SpawnFloor();
    auto *floorEvents = floor->AddComponent<EventRecorder>();
    const auto ball = SpawnBall("Ball", Vector3(0.0f, 2.0f, 0.0f));
    auto *ballEvents = ball->AddComponent<EventRecorder>();

    Step(120); // two seconds: the ball lands and rests

    EXPECT_GE(ballEvents->collisionEnter, 1) << "OnCollisionEnter never fired for solid contacts";
    EXPECT_GE(floorEvents->collisionEnter, 1);
    EXPECT_GT(ballEvents->collisionStay, 0);

    // Each side is told about itself and the other, not both about the same object
    EXPECT_EQ(ballEvents->lastCollisionSelf, ball.get());
    EXPECT_EQ(ballEvents->lastCollisionOther, floor.get());
    EXPECT_EQ(floorEvents->lastCollisionSelf, floor.get());
    EXPECT_EQ(floorEvents->lastCollisionOther, ball.get());
}

TEST_F(PhysicsTest, TriggerEventsFireOnBothBodies)
{
    const auto zone = Spawn("Zone", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(4.0f, 1.0f, 4.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();
    const auto ball = SpawnBall("Faller", Vector3(0.0f, 2.0f, 0.0f));
    auto *ballEvents = ball->AddComponent<EventRecorder>();

    Step(90);

    EXPECT_EQ(zoneEvents->triggerEnter, 1);
    EXPECT_EQ(ballEvents->triggerEnter, 1);
    EXPECT_EQ(zoneEvents->lastTriggerSelf, zone.get());
    EXPECT_EQ(zoneEvents->lastTriggerOther, ball.get());
    EXPECT_EQ(ballEvents->lastTriggerSelf, ball.get());
    EXPECT_EQ(ballEvents->lastTriggerOther, zone.get());
    EXPECT_EQ(ballEvents->collisionEnter, 0) << "a trigger must not be solid";
}

TEST_F(PhysicsTest, TriggerOnlyAffectsItsOwnCollider)
{
    // A solid box and a larger trigger sphere sharing one actor (via the Rigidbody). Toggling the
    // trigger used to flip every shape on the actor, turning the box into a trigger too.
    const auto post = Spawn("Post", Vector3(0.0f, 0.0f, 0.0f));
    post->AddComponent<BoxCollider>()->SetSize(Vector3(4.0f, 1.0f, 4.0f));
    auto *sensor = post->AddComponent<SphereCollider>();
    sensor->SetRadius(3.0f);
    sensor->SetIsTrigger(true);
    post->AddComponent<Rigidbody>()->SetBodyType(BodyType::Static);
    auto *postEvents = post->AddComponent<EventRecorder>();
    const auto ball = SpawnBall("Lander", Vector3(0.0f, 4.0f, 0.0f));

    Step(150);

    EXPECT_EQ(postEvents->triggerEnter, 1);
    EXPECT_GE(postEvents->collisionEnter, 1) << "the box turned into a trigger along with the sphere";
    EXPECT_GT(ball->GetPositionable()->GetPosition().y, 0.5f) << "the ball fell through the solid box";
}

TEST_F(PhysicsTest, HandlerRemovingRigidbodyDuringDispatchIsSafe)
{
    SpawnFloor();
    const auto ball = SpawnBall("SelfDestruct", Vector3(0.0f, 2.0f, 0.0f));
    auto *events = ball->AddComponent<EventRecorder>();
    events->onCollisionEnter = [&ball] { ball->RemoveComponent<Rigidbody>(); };

    Step(120);

    EXPECT_EQ(events->collisionEnter, 1);
    EXPECT_EQ(ball->GetComponent<Rigidbody>(), nullptr);
}

TEST_F(PhysicsTest, DestroyingObjectInsideTriggerIsSafe)
{
    const auto zone = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    // Dynamic without gravity, so it stays inside (PhysX doesn't pair kinematic with static by default)
    const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f));
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    auto *visitorBody = visitor->AddComponent<Rigidbody>();
    visitorBody->SetBodyType(BodyType::Dynamic);
    visitorBody->SetGravityEnabled(false);

    Step(5);
    ASSERT_EQ(zoneEvents->triggerEnter, 1);

    visitor->Destroy();
    Step(1); // destroyed at the end of this frame
    const int stays = zoneEvents->triggerStay;
    Step(5); // PhysX reports the lost touch with the released actor; must not be dereferenced

    EXPECT_EQ(zoneEvents->triggerStay, stays) << "the destroyed visitor still counts as inside";
    EXPECT_EQ(zoneEvents->triggerExit, 1) << "the zone never heard that the visitor left";
}

TEST_F(PhysicsTest, RecreatingATouchingColliderKeepsEnterAndExitBalanced)
{
    const auto zone = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f));
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    auto *visitorBody = visitor->AddComponent<Rigidbody>();
    visitorBody->SetBodyType(BodyType::Dynamic);
    visitorBody->SetGravityEnabled(false);

    Step(5);
    ASSERT_EQ(zoneEvents->triggerEnter, 1);

    // Remove and re-add in the same frame: the old shape's pair must end before the new one starts
    ASSERT_TRUE(visitor->RemoveComponent<SphereCollider>());
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    Step(5);

    // Used to be Enter, Enter with no Exit in between
    EXPECT_EQ(zoneEvents->triggerEnter, 2);
    EXPECT_EQ(zoneEvents->triggerExit, 1);
}

TEST_F(PhysicsTest, RemovingTheOverlappingShapeEndsTheTrigger)
{
    const auto zone = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(2.0f, 2.0f, 2.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    // Compound body: only the sphere overlaps the zone, the box sits well outside it
    const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f));
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    auto *farBox = visitor->AddComponent<BoxCollider>();
    farBox->SetSize(Vector3(0.5f, 0.5f, 0.5f));
    farBox->SetOffset(Vector3(10.0f, 0.0f, 0.0f));
    auto *visitorBody = visitor->AddComponent<Rigidbody>();
    visitorBody->SetBodyType(BodyType::Dynamic);
    visitorBody->SetGravityEnabled(false);

    Step(5);
    ASSERT_EQ(zoneEvents->triggerEnter, 1);

    ASSERT_TRUE(visitor->RemoveComponent<SphereCollider>());
    Step(1);
    const int stays = zoneEvents->triggerStay;
    Step(5);

    // The shape's touch-lost is flagged as a removed shape and skipped, so the pair used to stay active
    EXPECT_EQ(zoneEvents->triggerStay, stays) << "the removed sphere still counts as inside";
    EXPECT_EQ(zoneEvents->triggerExit, 1);
}

TEST_F(PhysicsTest, EachSideSeesImpulseAndRelativeVelocityFromItsOwnPerspective)
{
    const auto floor = SpawnFloor();
    auto *floorEvents = floor->AddComponent<EventRecorder>();
    const auto ball = SpawnBall("Lander", Vector3(0.0f, 2.0f, 0.0f));
    auto *ballEvents = ball->AddComponent<EventRecorder>();

    Step(120);

    ASSERT_GE(ballEvents->collisionEnter, 1);
    ASSERT_GE(floorEvents->collisionEnter, 1);
    const Vector3 ballImpulse = ballEvents->firstEnterImpulse;
    const Vector3 floorImpulse = floorEvents->firstEnterImpulse;
    const Vector3 ballVelocity = ballEvents->firstEnterRelativeVelocity;
    const Vector3 floorVelocity = floorEvents->firstEnterRelativeVelocity;
    ASSERT_GT(ballImpulse.Length() + ballVelocity.Length(), 1e-3f) << "nothing to compare";

    // Like the contact normals: B's values used to be A's, not negated
    ExpectNear(ballImpulse, floorImpulse * -1.0f);
    ExpectNear(ballVelocity, floorVelocity * -1.0f);
    EXPECT_GE(ballImpulse.y, -1e-4f) << "the floor pushes the ball up";
    EXPECT_LE(floorImpulse.y, 1e-4f);
}

// ============================================================================
// Inactive objects, disabled components, re-created bodies
// ============================================================================

TEST_F(PhysicsTest, DeactivatingAnObjectTakesItOutOfPhysicsUntilReactivated)
{
    const auto zone = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f));
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    auto *visitorBody = visitor->AddComponent<Rigidbody>();
    visitorBody->SetBodyType(BodyType::Dynamic);
    visitorBody->SetGravityEnabled(false);
    auto *visitorEvents = visitor->AddComponent<EventRecorder>();

    Step(5);
    ASSERT_EQ(zoneEvents->triggerEnter, 1);
    ASSERT_EQ(visitorEvents->triggerEnter, 1);

    visitor->SetActive(false);
    Step(1);
    const int zoneStays = zoneEvents->triggerStay;
    const int visitorCallbacks = visitorEvents->Total();
    Step(5);

    EXPECT_EQ(zoneEvents->triggerExit, 1) << "the zone never heard that the visitor left";
    EXPECT_EQ(zoneEvents->triggerStay, zoneStays) << "the inactive visitor still counts as inside";
    EXPECT_EQ(visitorEvents->Total(), visitorCallbacks) << "an inactive object got physics callbacks";
    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1) << "only the zone may be hit, not the inactive visitor";

    visitor->SetActive(true);
    Step(5);

    EXPECT_EQ(zoneEvents->triggerEnter, 2) << "still overlapping, so re-enabling enters again";
    EXPECT_EQ(visitorEvents->triggerEnter, 2);
    EXPECT_GT(zoneEvents->triggerStay, zoneStays);
}

TEST_F(PhysicsTest, InactiveBodyDoesNotSimulateAndResumesAtItsObjectsPose)
{
    const auto ball = SpawnBall("Sleeper", Vector3(0.0f, 10.0f, 0.0f));
    Step(1);
    ball->SetActive(false);
    const float heightWhenDeactivated = ball->GetPositionable()->GetPosition().y;

    Step(30);
    EXPECT_FLOAT_EQ(ball->GetPositionable()->GetPosition().y, heightWhenDeactivated) << "an inactive body kept falling";

    // Moved while out of the simulation: it carries on from there, not from where it was taken out
    ball->GetPositionable()->SetPosition(Vector3(3.0f, 20.0f, 0.0f));
    ball->SetActive(true);
    Step(1);

    const Vector3 resumed = ball->GetPositionable()->GetPosition();
    EXPECT_NEAR(resumed.x, 3.0f, 1e-3f);
    EXPECT_GT(resumed.y, 19.0f);
    EXPECT_LT(resumed.y, 20.0f) << "it should be falling again";
}

TEST_F(PhysicsTest, ObjectAttachedWhileInactiveStaysOutOfPhysics)
{
    const auto go = Spawn("BornInactive", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    go->SetActive(false);
    Step(1); // attaches while inactive

    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 0);

    go->SetActive(true);
    Step(1);
    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1);
}

TEST_F(PhysicsTest, DisablingAColliderEndsItsPairsAndEnablingRestoresThem)
{
    const auto zone = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f));
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    auto *visitorBody = visitor->AddComponent<Rigidbody>();
    visitorBody->SetBodyType(BodyType::Dynamic);
    visitorBody->SetGravityEnabled(false);
    auto *visitorEvents = visitor->AddComponent<EventRecorder>();

    Step(5);
    ASSERT_EQ(visitorEvents->triggerEnter, 1);

    zoneCollider->SetActive(false);
    Step(5);

    // The zone object is still active, so both sides hear the pair end
    EXPECT_EQ(visitorEvents->triggerExit, 1);
    EXPECT_EQ(zoneEvents->triggerExit, 1);
    EXPECT_EQ(ShapesUnder(Vector3(1.5f, 5.0f, 0.0f)), 0) << "a disabled collider is still hit by queries";

    zoneCollider->SetActive(true);
    Step(5);

    EXPECT_EQ(visitorEvents->triggerEnter, 2);
    EXPECT_EQ(zoneEvents->triggerEnter, 2);
    EXPECT_TRUE(zoneCollider->IsTrigger()) << "re-enabled shapes lost the trigger setting";
}

TEST_F(PhysicsTest, DisabledComponentGetsNoPhysicsCallbacks)
{
    SpawnFloor();
    const auto ball = SpawnBall("Muted", Vector3(0.0f, 2.0f, 0.0f));
    auto *muted = ball->AddComponent<EventRecorder>();
    auto *listening = ball->AddComponent<EventRecorder>();
    muted->SetActive(false);

    Step(120);

    EXPECT_GE(listening->collisionEnter, 1);
    EXPECT_EQ(muted->Total(), 0);
}

TEST_F(PhysicsTest, RecreatingABodyKeepsEnterAndExitBalancedOnBothSides)
{
    // A weightless dynamic zone, so the visitor still pairs with it once it's kinematic (PhysX doesn't
    // pair kinematic with static by default)
    const auto zone = Spawn("Volume", Vector3(0.0f, 0.0f, 0.0f));
    auto *zoneCollider = zone->AddComponent<BoxCollider>();
    zoneCollider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
    zoneCollider->SetIsTrigger(true);
    auto *zoneBody = zone->AddComponent<Rigidbody>();
    zoneBody->SetBodyType(BodyType::Dynamic);
    zoneBody->SetGravityEnabled(false);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    const auto visitor = Spawn("Visitor", Vector3(0.0f, 0.0f, 0.0f));
    visitor->AddComponent<SphereCollider>()->SetRadius(0.5f);
    auto *visitorBody = visitor->AddComponent<Rigidbody>();
    visitorBody->SetBodyType(BodyType::Dynamic);
    visitorBody->SetGravityEnabled(false);
    auto *visitorEvents = visitor->AddComponent<EventRecorder>();

    Step(5);
    ASSERT_EQ(visitorEvents->triggerEnter, 1);
    ASSERT_EQ(zoneEvents->triggerEnter, 1);

    visitorBody->SetBodyType(BodyType::Kinematic); // destroys the body and creates a new one in place
    Step(5);

    // The visitor's own components used to get Enter again with no Exit for the old body
    EXPECT_EQ(visitorEvents->triggerExit, 1);
    EXPECT_EQ(visitorEvents->triggerEnter, 2);
    EXPECT_EQ(visitorEvents->lastTriggerExitSelf, visitor.get()) << "the Exit must name the object it went to";
    EXPECT_EQ(zoneEvents->triggerExit, 1);
    EXPECT_EQ(zoneEvents->triggerEnter, 2);
}

// ============================================================================
// Scene queries
// ============================================================================

TEST_F(PhysicsTest, RaycastHitsWithDefaultLayerMask)
{
    const auto target = Spawn("Target", Vector3(0.0f, 0.0f, 5.0f));
    target->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    Step(1);

    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), hit, 100.0f))
        << "shapes had zero query filter data, so the default mask filtered out everything";
    EXPECT_EQ(hit.gameObject, target.get());
    EXPECT_NEAR(hit.distance, 4.5f, 1e-3f);

    EXPECT_TRUE(Raycast::Single(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), hit, 100.0f, 0x1u));
}

TEST_F(PhysicsTest, RaycastAllReturnsEveryHit)
{
    const auto near = Spawn("Near", Vector3(0.0f, 0.0f, 3.0f));
    near->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    const auto far = Spawn("Far", Vector3(0.0f, 0.0f, 8.0f));
    far->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    Step(1);

    std::vector<RaycastHit> hits;
    ASSERT_EQ(Raycast::All(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f), hits, 100.0f), 2);
    EXPECT_EQ(hits[0].gameObject, near.get());
    EXPECT_EQ(hits[1].gameObject, far.get());
}

TEST_F(PhysicsTest, QueriesReportTheColliderThatWasHit)
{
    // One body, two colliders: a box at the origin and a sphere off to the side
    const auto compound = Spawn("Compound", Vector3(0.0f, 0.0f, 0.0f));
    auto *box = compound->AddComponent<BoxCollider>();
    box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *sphere = compound->AddComponent<SphereCollider>();
    sphere->SetRadius(0.5f);
    sphere->SetOffset(Vector3(5.0f, 0.0f, 0.0f));
    auto *body = compound->AddComponent<Rigidbody>();
    body->SetBodyType(BodyType::Kinematic);
    const auto wall = Spawn("Wall", Vector3(-5.0f, 0.0f, 0.0f));
    auto *wallBox = wall->AddComponent<BoxCollider>();
    wallBox->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    Step(1);

    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(Vector3(5.0f, 5.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), hit, 100.0f));
    EXPECT_EQ(hit.collider, sphere) << "with a Rigidbody the collider used to be null";
    EXPECT_EQ(hit.rigidbody, body);
    EXPECT_EQ(hit.gameObject, compound.get());

    ASSERT_TRUE(Raycast::SphereCast(Vector3(0.0f, 5.0f, 0.0f), 0.1f, Vector3(0.0f, -1.0f, 0.0f), hit, 100.0f));
    EXPECT_EQ(hit.collider, box);
    EXPECT_EQ(hit.rigidbody, body);

    // A reused hit keeps nothing from the previous one
    ASSERT_TRUE(Raycast::Single(Vector3(-5.0f, 5.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), hit, 100.0f));
    EXPECT_EQ(hit.collider, wallBox);
    EXPECT_EQ(hit.rigidbody, nullptr) << "the previous hit's Rigidbody leaked into this one";
    EXPECT_EQ(hit.gameObject, wall.get());

    EXPECT_FALSE(Raycast::Single(Vector3(20.0f, 5.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), hit, 100.0f));
    EXPECT_FALSE(hit.hit);
    EXPECT_EQ(hit.collider, nullptr);
    EXPECT_EQ(hit.gameObject, nullptr);
    EXPECT_FALSE(hit.bodyHandle.IsValid());
}

// ============================================================================
// Collider shapes
// ============================================================================

TEST_F(PhysicsTest, ColliderThenRigidbodyGivesOneShape)
{
    const auto go = Spawn("ColliderFirst", Vector3(0.0f, 0.0f, 0.0f));
    auto *box = go->AddComponent<BoxCollider>();
    box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    go->AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
    Step(1);

    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1);

    box->SetSize(Vector3(2.0f, 2.0f, 2.0f)); // used a shape the old static body had freed
    Step(1);
    RaycastHit hit;
    ASSERT_TRUE(Raycast::Single(Vector3(0.0f, 5.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), hit, 100.0f));
    EXPECT_NEAR(hit.distance, 4.0f, 1e-3f) << "the resize didn't reach the live shape";
}

TEST_F(PhysicsTest, RigidbodyThenColliderGivesOneShape)
{
    const auto go = Spawn("RigidbodyFirst", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    Step(1);

    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1) << "the shape was attached twice";
}

TEST_F(PhysicsTest, ChangingBodyTypeKeepsOneShape)
{
    const auto go = Spawn("Retyped", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *body = go->AddComponent<Rigidbody>();
    body->SetBodyType(BodyType::Kinematic);
    Step(1);

    body->SetBodyType(BodyType::Static); // recreates the body
    Step(1);

    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1);
}

TEST_F(PhysicsTest, RemovingColliderRemovesItsShape)
{
    const auto go = Spawn("Stripped", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    Step(1);
    ASSERT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1);

    go->RemoveComponent<BoxCollider>();
    Step(1);

    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 0) << "the shape stayed on the actor as a ghost";
}

// ============================================================================
// Transforms
// ============================================================================

TEST_F(PhysicsTest, KinematicBodyFollowsLatestTransform)
{
    const auto go = Spawn("Mover", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *body = go->AddComponent<Rigidbody>();
    body->SetBodyType(BodyType::Kinematic);
    Step(1);

    // Two moves before anything reads the transform: the second one used to be dropped, and the
    // first reached physics as the previous (stale) pose
    go->GetPositionable()->SetPosition(Vector3(1.0f, 0.0f, 0.0f));
    go->GetPositionable()->SetPosition(Vector3(3.0f, 0.0f, 0.0f));
    Step(1);

    const Vector3 physicsPos = Backend().GetPosition(body->GetHandle());
    EXPECT_NEAR(physicsPos.x, 3.0f, 1e-4f);
    EXPECT_NEAR(go->GetPositionable()->GetPosition().x, 3.0f, 1e-4f) << "the physics pose overwrote the script's move";
}

TEST_F(PhysicsTest, SceneSwitchReleasesDynamicBodies)
{
    // The review's scenario: switch scenes with a dynamic body, then run a fixed step
    const auto ball = SpawnBall("Orphan", Vector3(0.0f, 10.0f, 0.0f));
    Step(1);
    const float heightAtSwitch = ball->GetPositionable()->GetPosition().y;

    SceneManager::AddScene(Scene::Create("Physics_AfterSwitch"), true);
    SceneManager::ProcessAnyPendingSceneChange();
    _scene = SceneManager::GetCurScene();
    Step(30); // SyncTransforms used to reach the old scene's Rigidbody

    EXPECT_FLOAT_EQ(ball->GetPositionable()->GetPosition().y, heightAtSwitch)
        << "the old scene's body kept simulating";
    EXPECT_FALSE(ball->GetComponent<Rigidbody>()->GetHandle().IsValid());
}

// ============================================================================
// Collider identity, compound bodies, removed Rigidbody
// ============================================================================

TEST_F(PhysicsTest, CollisionReportsCollidersOnEachSide)
{
    const auto floor = SpawnFloor();
    auto *floorEvents = floor->AddComponent<EventRecorder>();
    const auto ball = SpawnBall("Identified", Vector3(0.0f, 2.0f, 0.0f));
    auto *ballEvents = ball->AddComponent<EventRecorder>();

    Step(120);

    ASSERT_GE(ballEvents->collisionEnter, 1);
    EXPECT_EQ(ballEvents->lastCollisionCollider, ball->GetComponent<SphereCollider>());
    EXPECT_EQ(ballEvents->lastCollisionOtherCollider, floor->GetComponent<BoxCollider>());
    EXPECT_EQ(floorEvents->lastCollisionCollider, floor->GetComponent<BoxCollider>());
    EXPECT_EQ(floorEvents->lastCollisionOtherCollider, ball->GetComponent<SphereCollider>());
}

TEST_F(PhysicsTest, CompoundTriggerExitsOnlyWhenFullyOutside)
{
    // Two overlapping trigger boxes on one body: x in [-2.5, 0.5] and [-0.5, 2.5]
    const auto zone = Spawn("CompoundZone", Vector3(0.0f, 0.0f, 0.0f));
    auto *left = zone->AddComponent<BoxCollider>();
    left->SetSize(Vector3(3.0f, 1.0f, 1.0f));
    left->SetOffset(Vector3(-1.0f, 0.0f, 0.0f));
    left->SetIsTrigger(true);
    auto *right = zone->AddComponent<SphereCollider>(); // a different type, since GetComponent finds one per type
    right->SetRadius(1.25f);
    right->SetOffset(Vector3(1.25f, 0.0f, 0.0f));        // x in [0, 2.5], overlapping the box
    right->SetIsTrigger(true);
    zone->AddComponent<Rigidbody>()->SetBodyType(BodyType::Static);
    auto *zoneEvents = zone->AddComponent<EventRecorder>();

    // A ball flying along +x through both
    const auto ball = Spawn("Flyer", Vector3(-5.0f, 0.0f, 0.0f));
    ball->AddComponent<SphereCollider>()->SetRadius(0.2f);
    auto *body = ball->AddComponent<Rigidbody>();
    body->SetBodyType(BodyType::Dynamic);
    body->SetGravityEnabled(false);
    Step(1);
    body->SetVelocity(Vector3(5.0f, 0.0f, 0.0f));

    bool checkedOnlyInSecond = false;
    for (int i = 0; i < 180 && ball->GetPositionable()->GetPosition().x < 5.0f; ++i)
    {
        Step(1);
        const float x = ball->GetPositionable()->GetPosition().x;
        if (x > 1.2f && x < 2.0f) // clear of the box, still inside the sphere
        {
            EXPECT_EQ(zoneEvents->triggerExit, 0) << "exited while still inside the other shape (x=" << x << ")";
            checkedOnlyInSecond = true;
        }
    }

    ASSERT_TRUE(checkedOnlyInSecond) << "the ball never reached the second shape";
    EXPECT_EQ(zoneEvents->triggerEnter, 1);
    EXPECT_EQ(zoneEvents->triggerExit, 1);
    EXPECT_EQ(zoneEvents->firstTriggerCollider, left) << "the box is entered first";
    EXPECT_EQ(zoneEvents->firstTriggerOtherCollider, ball->GetComponent<SphereCollider>());
}

TEST_F(PhysicsTest, RemovingRigidbodyLeavesStaticCollider)
{
    // Like Unity: the remaining collider becomes a static collider instead of losing its body
    const auto go = Spawn("Grounded", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *body = go->AddComponent<Rigidbody>();
    body->SetBodyType(BodyType::Dynamic);
    body->SetGravityEnabled(false);
    Step(1);
    ASSERT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1);

    go->RemoveComponent<Rigidbody>();
    Step(1);

    EXPECT_EQ(ShapesUnder(Vector3(0.0f, 5.0f, 0.0f)), 1) << "the collider was left without a body";
    EXPECT_TRUE(go->GetComponent<BoxCollider>()->GetHandle().IsValid());
}

// ============================================================================
// Mass properties (inertia and centre of mass follow the colliders; the mass is the Rigidbody's)
// ============================================================================

namespace
{
    PhysXBackend &PhysX() { return static_cast<PhysXBackend &>(*Application::GetInstance().Get3DPhysicsBackend()); }

    void ExpectVectorNear(const Vector3 &actual, const Vector3 &expected, const float tolerance)
    {
        EXPECT_NEAR(actual.x, expected.x, tolerance);
        EXPECT_NEAR(actual.y, expected.y, tolerance);
        EXPECT_NEAR(actual.z, expected.z, tolerance);
    }

    // Solid box of full size s and mass m: I = m/12 * (sy^2 + sz^2, sx^2 + sz^2, sx^2 + sy^2)
    Vector3 BoxInertia(const float mass, const Vector3 &size)
    {
        return Vector3(size.y * size.y + size.z * size.z,
                       size.x * size.x + size.z * size.z,
                       size.x * size.x + size.y * size.y) * (mass / 12.0f);
    }

    // x of the centre of mass of a unit cube at the origin plus a radius-0.5 sphere at x = 2, same density
    constexpr float CubeAndSphereCenterX = 0.6873f; // 2 * (pi / 6) / (1 + pi / 6)
}

class PhysicsMassTest : public PhysicsTest
{
protected:
    // Dynamic and weightless (so it stays put). The body is only created when the attach queue runs
    // in the next Step, so the mass here is the one it's created with.
    Rigidbody *AddFloatingBody(GameObject &go, const float mass)
    {
        auto *body = go.AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        body->SetGravityEnabled(false);
        body->SetMass(mass);
        return body;
    }
};

TEST_F(PhysicsMassTest, BodyWithoutCollidersKeepsItsMass)
{
    const auto go = Spawn("Shapeless", Vector3(0.0f, 0.0f, 0.0f));
    auto *body = AddFloatingBody(*go, 3.0f);
    Step(1);

    // Fallback: the configured mass, a centred COM, and inertia that scales with the mass (PhysX's own
    // shapeless default is (1,1,1) whatever the mass)
    EXPECT_NEAR(body->GetMass(), 3.0f, 1e-4f);
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), Vector3(3.0f, 3.0f, 3.0f), 1e-4f);
}

TEST_F(PhysicsMassTest, InertiaAndCenterFollowBoxSizeAndOffset)
{
    const auto go = Spawn("Plank", Vector3(0.0f, 0.0f, 0.0f));
    auto *box = go->AddComponent<BoxCollider>();
    box->SetSize(Vector3(1.0f, 2.0f, 3.0f));
    auto *body = AddFloatingBody(*go, 2.0f);
    Step(1);

    EXPECT_NEAR(body->GetMass(), 2.0f, 1e-4f) << "the collider's volume must not override the Rigidbody's mass";
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(2.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);

    box->SetOffset(Vector3(0.0f, 1.0f, 0.0f));
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 1.0f, 0.0f), 1e-4f);

    box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(2.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);
    EXPECT_NEAR(body->GetMass(), 2.0f, 1e-4f);
}

TEST_F(PhysicsMassTest, ChangingMassScalesInertia)
{
    const auto go = Spawn("Scaled", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 2.0f, 3.0f));
    auto *body = AddFloatingBody(*go, 2.0f);
    Step(1);

    body->SetMass(4.0f);

    EXPECT_NEAR(body->GetMass(), 4.0f, 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(4.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);
}

TEST_F(PhysicsMassTest, LongBoxHasLessInertiaAboutItsLongAxis)
{
    // Equal masses: the rod spins most easily about its long axis, the cube the same about every axis
    const auto rod = Spawn("Rod", Vector3(0.0f, 0.0f, 0.0f));
    rod->AddComponent<BoxCollider>()->SetSize(Vector3(4.0f, 0.5f, 0.5f));
    auto *rodBody = AddFloatingBody(*rod, 1.0f);
    const auto cube = Spawn("Cube", Vector3(0.0f, 5.0f, 0.0f));
    cube->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *cubeBody = AddFloatingBody(*cube, 1.0f);
    Step(1);

    const Vector3 rodInertia = PhysX().GetInertiaTensor(rodBody->GetHandle());
    const Vector3 cubeInertia = PhysX().GetInertiaTensor(cubeBody->GetHandle());
    EXPECT_LT(rodInertia.x, cubeInertia.x);
    EXPECT_GT(rodInertia.y, cubeInertia.y);
    EXPECT_LT(rodInertia.x * 10.0f, rodInertia.y) << "inertia isn't following the collider's shape";
}

TEST_F(PhysicsMassTest, CapsuleHasLeastInertiaAboutItsAxis)
{
    // The capsule shape is rotated onto Y, so PhysX may report the principal moments in any order:
    // compare them sorted. Two are equal (across the axis), the one about the axis is much smaller.
    const auto go = Spawn("Pill", Vector3(0.0f, 0.0f, 0.0f));
    auto *capsule = go->AddComponent<CapsuleCollider>();
    capsule->SetRadius(0.25f);
    capsule->SetHeight(2.0f);
    capsule->SetOffset(Vector3(0.0f, 0.5f, 0.0f));
    auto *body = AddFloatingBody(*go, 1.0f);
    Step(1);

    const Vector3 inertia = PhysX().GetInertiaTensor(body->GetHandle());
    std::array<float, 3> moments{inertia.x, inertia.y, inertia.z};
    std::ranges::sort(moments);
    EXPECT_GT(moments[0], 0.0f);
    EXPECT_LT(moments[0] * 4.0f, moments[1]);
    EXPECT_NEAR(moments[1], moments[2], moments[2] * 1e-3f);
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.5f, 0.0f), 1e-4f);
}

TEST_F(PhysicsMassTest, InvalidDeserializedMassFallsBackToOne)
{
    const auto go = Spawn("Massless", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *body = AddFloatingBody(*go, 2.0f);
    nlohmann::json data;
    data["_mass"] = 0.0f;
    body->Deserialize(data, nullptr); // unlike SetMass, doesn't clamp
    Step(1);

    EXPECT_NEAR(body->GetMass(), 1.0f, 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(1.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);

    body->AddImpulse(Vector3(1.0f, 0.0f, 0.0f));
    Step(1);
    EXPECT_NEAR(body->GetVelocity().x, 1.0f, 0.05f) << "a zero mass made the dynamic body immovable";
}

TEST_F(PhysicsMassTest, OffsetColliderMovesCenterOfMass)
{
    const auto go = Spawn("Compound", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *body = AddFloatingBody(*go, 1.0f);
    Step(1);
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);

    auto *sphere = go->AddComponent<SphereCollider>();
    sphere->SetRadius(0.5f);
    sphere->SetOffset(Vector3(2.0f, 0.0f, 0.0f));
    Step(1);

    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(CubeAndSphereCenterX, 0.0f, 0.0f), 1e-3f);
    EXPECT_NEAR(body->GetMass(), 1.0f, 1e-4f);

    ASSERT_TRUE(go->RemoveComponent<SphereCollider>());
    Step(1);
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(1.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);
}

TEST_F(PhysicsMassTest, TriggerColliderDoesNotAffectMassProperties)
{
    const auto go = Spawn("Sensored", Vector3(0.0f, 0.0f, 0.0f));
    go->AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
    auto *sensor = go->AddComponent<SphereCollider>();
    sensor->SetRadius(0.5f);
    sensor->SetOffset(Vector3(2.0f, 0.0f, 0.0f));
    sensor->SetIsTrigger(true);
    auto *body = AddFloatingBody(*go, 1.0f);
    Step(1);

    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(1.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);

    sensor->SetIsTrigger(false); // now solid, so it counts
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(CubeAndSphereCenterX, 0.0f, 0.0f), 1e-3f);
}

TEST_F(PhysicsMassTest, ChangingBodyTypeKeepsMassProperties)
{
    const auto go = Spawn("Retyped", Vector3(0.0f, 0.0f, 0.0f));
    auto *box = go->AddComponent<BoxCollider>();
    box->SetSize(Vector3(1.0f, 2.0f, 3.0f));
    box->SetOffset(Vector3(0.0f, 0.5f, 0.0f));
    auto *body = AddFloatingBody(*go, 2.0f);
    Step(1);

    body->SetBodyType(BodyType::Kinematic); // recreates the body
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.5f, 0.0f), 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(2.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);

    body->SetBodyType(BodyType::Dynamic);
    Step(1);

    EXPECT_NEAR(body->GetMass(), 2.0f, 1e-4f);
    ExpectVectorNear(PhysX().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.5f, 0.0f), 1e-4f);
    ExpectVectorNear(PhysX().GetInertiaTensor(body->GetHandle()), BoxInertia(2.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);
}

#endif // N2ENGINE_PHYSX_ENABLED
