// 2D physics through the engine: components, the fixed step (Application::StepTicks, with no 3D backend installed), events,
// layers, queries and serialization. The rows of "Behaviour of Box2D that the contracts depend on" in
// orchestrator/proposals/physics2d-box2d-design.md each have a test here. With N2ENGINE_USE_BOX2D=OFF only the stripped-build
// tests at the end are compiled (docs/physics2d.html#stripping).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector2.hpp>
#include <math/Vector3.hpp>

#include "engine/Application.hpp"
#include "engine/Component.hpp"
#include "engine/GameObject.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Layers.hpp"
#include "engine/Positionable.hpp"
#include "engine/physics2d/Collider2D.hpp"
#include "engine/physics2d/Physics2DQueries.hpp"
#include "engine/physics2d/Physics2DWorld.hpp"
#include "engine/physics2d/Rigidbody2D.hpp"
#include "engine/prefabs/PrefabManager.hpp"
#include "engine/sceneManagement/SceneManager.hpp"
#include "engine/serialization/ComponentRegistry.hpp"

using namespace N2Engine;
using namespace N2Engine::Physics2D;
using Math::Vector2;
using Math::Vector3;

namespace
{
    // A headless scene, stepped by the real Application::StepTicks
    class Physics2DSceneTest : public ::testing::Test
    {
    protected:
        Scene *_scene = nullptr;

        void SetUp() override
        {
            Layers::ResetToDefaults();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
            SceneManager::AddScene(Scene::Create(std::string("Physics2D_") + ::testing::UnitTest::GetInstance()->current_test_info()->name()), true);
            SceneManager::ProcessAnyPendingSceneChange();
            _scene = SceneManager::GetCurScene();
        }

        void TearDown() override
        {
            SceneManager::AddScene(Scene::Create("Physics2D_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Layers::ResetToDefaults();
            Physics2DWorld::SetGravity(Vector2(0.0f, -9.81f));
        }

        void Step(const int steps = 1)
        {
            for (int i = 0; i < steps; ++i)
            {
                _scene->ProcessAttachQueue();
                ASSERT_EQ(Application::GetInstance().StepTicks(1), 1u);
                _scene->ProcessDestroyed();
            }
        }

        GameObject::Ptr Make(const std::string &name, const float x, const float y)
        {
            auto go = GameObject::Create(name);
            go->CreatePositionable();
            go->GetPositionable()->SetPosition(Vector3(x, y, 0.0f));
            _scene->AddRootGameObject(go);
            return go;
        }

        Rigidbody2D *MakeBody(const GameObject::Ptr &go, const BodyType2D type, const float gravityScale = 1.0f)
        {
            auto *rb = go->AddComponent<Rigidbody2D>();
            rb->SetBodyType(type);
            rb->SetGravityScale(gravityScale);
            return rb;
        }

        GameObject::Ptr MakeFloor()
        {
            auto floor = Make("Floor", 0.0f, -1.0f);
            floor->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(50.0f, 1.0f)); // its top is at y = 0
            return floor;
        }

        [[nodiscard]] static Vector3 PositionOf(const GameObject::Ptr &go) { return go->GetPositionable()->GetPosition(); }
    };

#ifdef N2ENGINE_BOX2D_ENABLED

    // Counts the events its object gets and keeps the latest collision
    class Recorder final : public Component
    {
    public:
        explicit Recorder(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "Recorder2D"; }

        void OnCollisionEnter2D(const Collision2D &c) override
        {
            ++collisionEnter;
            last = c;
            order.push_back("collisionEnter");
        }
        void OnCollisionStay2D(const Collision2D &) override
        {
            ++collisionStay;
            order.push_back("collisionStay");
        }
        void OnCollisionExit2D(const Collision2D &c) override
        {
            ++collisionExit;
            last = c;
            order.push_back("collisionExit");
        }
        void OnTriggerEnter2D(const Trigger2D &t) override
        {
            ++triggerEnter;
            lastTrigger = t;
            order.push_back("triggerEnter");
        }
        void OnTriggerStay2D(const Trigger2D &) override
        {
            ++triggerStay;
            order.push_back("triggerStay");
        }
        void OnTriggerExit2D(const Trigger2D &) override
        {
            ++triggerExit;
            order.push_back("triggerExit");
        }

        int collisionEnter = 0, collisionStay = 0, collisionExit = 0;
        int triggerEnter = 0, triggerStay = 0, triggerExit = 0;
        Collision2D last;
        Trigger2D lastTrigger;
        std::vector<std::string> order;
    };

    using Physics2DTest = Physics2DSceneTest;

    // ===== Bodies and shapes =====

    TEST_F(Physics2DTest, NoWorldUntilA2DComponentAttaches)
    {
        Make("Plain", 0.0f, 0.0f);
        Step(2);
        EXPECT_EQ(_scene->GetPhysics2DWorld(), nullptr);

        Make("Body", 0.0f, 0.0f)->AddComponent<CircleCollider2D>();
        Step();
        EXPECT_NE(_scene->GetPhysics2DWorld(), nullptr);
    }

    TEST_F(Physics2DTest, EachShapeFallsAndRestsOnAStaticFloor)
    {
        MakeFloor();
        auto box = Make("Box", -4.0f, 3.0f);
        MakeBody(box, BodyType2D::Dynamic);
        box->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.5f, 0.5f));
        auto circle = Make("Circle", 0.0f, 3.0f);
        MakeBody(circle, BodyType2D::Dynamic);
        circle->AddComponent<CircleCollider2D>()->SetRadius(0.5f);
        auto polygon = Make("Polygon", 4.0f, 3.0f);
        MakeBody(polygon, BodyType2D::Dynamic);
        polygon->AddComponent<PolygonCollider2D>()->SetVertices({{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}});

        Step(300);

        EXPECT_NEAR(PositionOf(box).y, 0.5f, 0.05f);
        EXPECT_NEAR(PositionOf(circle).y, 0.5f, 0.05f);
        EXPECT_NEAR(PositionOf(polygon).y, 0.5f, 0.05f);
    }

    TEST_F(Physics2DTest, ZIsLeftAloneAndRotationIsAboutZ)
    {
        auto go = Make("Spinner", 1.0f, 2.0f);
        go->GetPositionable()->SetPosition(Vector3(1.0f, 2.0f, 7.5f));
        auto *rb = MakeBody(go, BodyType2D::Dynamic, 0.0f);
        go->AddComponent<CircleCollider2D>();
        rb->SetAngularVelocity(1.0f);
        rb->SetLinearVelocity(Vector2(1.0f, 0.0f));

        Step(50); // one second

        const Vector3 position = PositionOf(go);
        EXPECT_FLOAT_EQ(position.z, 7.5f);
        EXPECT_NEAR(position.x, 2.0f, 0.02f);
        const Math::Quaternion rotation = go->GetPositionable()->GetRotation();
        EXPECT_FLOAT_EQ(rotation.GetX(), 0.0f);
        EXPECT_FLOAT_EQ(rotation.GetY(), 0.0f);
        EXPECT_NEAR(2.0f * std::atan2(rotation.GetZ(), rotation.GetW()), 1.0f, 0.02f);
    }

    TEST_F(Physics2DTest, VelocitySetBeforeTheBodyExistsIsApplied)
    {
        auto go = Make("Projectile", 0.0f, 0.0f);
        auto *rb = MakeBody(go, BodyType2D::Dynamic, 0.0f);
        go->AddComponent<CircleCollider2D>();
        rb->SetLinearVelocity(Vector2(3.0f, 0.0f));
        Step(50);
        EXPECT_NEAR(PositionOf(go).x, 3.0f, 0.05f);
    }

    TEST_F(Physics2DTest, FixedRotationAndDampingAndGravityScale)
    {
        auto fixed = Make("Fixed", 0.0f, 0.0f);
        auto *fixedBody = MakeBody(fixed, BodyType2D::Dynamic, 0.0f);
        fixed->AddComponent<CircleCollider2D>();
        fixedBody->SetFixedRotation(true);
        Step();
        fixedBody->AddAngularImpulse(5.0f);
        Step(10);
        EXPECT_FLOAT_EQ(fixedBody->GetAngularVelocity(), 0.0f);

        auto damped = Make("Damped", 5.0f, 0.0f);
        auto *dampedBody = MakeBody(damped, BodyType2D::Dynamic, 0.0f);
        damped->AddComponent<CircleCollider2D>();
        dampedBody->SetLinearDamping(5.0f);
        dampedBody->SetLinearVelocity(Vector2(4.0f, 0.0f));
        Step(50);
        EXPECT_LT(dampedBody->GetLinearVelocity().x, 0.1f);

        auto floating = Make("Floating", 10.0f, 0.0f);
        MakeBody(floating, BodyType2D::Dynamic, 0.0f);
        floating->AddComponent<CircleCollider2D>();
        Step(20);
        EXPECT_FLOAT_EQ(PositionOf(floating).y, 0.0f);
    }

    TEST_F(Physics2DTest, MassComesFromShapesAndCanBeOverridden)
    {
        auto go = Make("Crate", 0.0f, 0.0f);
        auto *rb = MakeBody(go, BodyType2D::Dynamic, 0.0f);
        auto *box = go->AddComponent<BoxCollider2D>();
        box->SetHalfExtents(Vector2(1.0f, 1.0f)); // area 4
        box->SetDensity(2.0f);
        Step();
        EXPECT_NEAR(rb->GetMass(), 8.0f, 0.01f);

        rb->SetMass(3.0f);
        EXPECT_NEAR(rb->GetMass(), 3.0f, 0.001f);
        EXPECT_FALSE(rb->UsesAutoMass());

        rb->UseAutoMass();
        EXPECT_NEAR(rb->GetMass(), 8.0f, 0.01f);
    }

    TEST_F(Physics2DTest, ATriggerAddsNoMassAndATriggerOnlyBodyStillRespondsToForces)
    {
        auto solidAndTrigger = Make("Mixed", 0.0f, 0.0f);
        auto *mixed = MakeBody(solidAndTrigger, BodyType2D::Dynamic, 0.0f);
        solidAndTrigger->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(1.0f, 1.0f));
        auto *sensor = solidAndTrigger->AddComponent<CircleCollider2D>();
        sensor->SetRadius(5.0f);
        sensor->SetIsTrigger(true);
        Step();
        EXPECT_NEAR(mixed->GetMass(), 4.0f, 0.01f);

        auto triggerOnly = Make("TriggerOnly", 20.0f, 0.0f);
        auto *body = MakeBody(triggerOnly, BodyType2D::Dynamic, 1.0f);
        triggerOnly->AddComponent<CircleCollider2D>()->SetIsTrigger(true);
        Step(20);
        EXPECT_GT(body->GetMass(), 0.0f);
        EXPECT_LT(PositionOf(triggerOnly).y, -0.1f);
    }

    TEST_F(Physics2DTest, BadGeometryMakesNoShapeInsteadOfCrashing)
    {
        auto go = Make("Bad", 0.0f, 0.0f);
        auto *rb = MakeBody(go, BodyType2D::Dynamic, 0.0f);
        go->AddComponent<PolygonCollider2D>()->SetVertices({{0.0f, 0.0f}, {1.0f, 0.0f}});
        go->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(-1.0f, 1.0f));
        go->AddComponent<CircleCollider2D>()->SetRadius(0.0f);
        Step(3);
        EXPECT_GT(rb->GetMass(), 0.0f);
    }

    TEST_F(Physics2DTest, KinematicTransformMoveCarriesToThePoseAndStops)
    {
        auto platform = Make("Platform", 0.0f, 0.0f);
        auto *rb = MakeBody(platform, BodyType2D::Kinematic);
        platform->AddComponent<BoxCollider2D>();
        Step();

        platform->GetPositionable()->SetPosition(Vector3(1.0f, 0.0f, 0.0f));
        Step();
        EXPECT_NEAR(PositionOf(platform).x, 1.0f, 0.01f);
        EXPECT_FLOAT_EQ(rb->GetLinearVelocity().x, 0.0f);

        Step(10);
        EXPECT_NEAR(PositionOf(platform).x, 1.0f, 0.01f);
    }

    TEST_F(Physics2DTest, KinematicMovesByItsVelocityAndIgnoresGravity)
    {
        auto platform = Make("Platform", 0.0f, 0.0f);
        auto *rb = MakeBody(platform, BodyType2D::Kinematic);
        platform->AddComponent<BoxCollider2D>();
        rb->SetLinearVelocity(Vector2(2.0f, 0.0f));
        Step(50);
        EXPECT_NEAR(PositionOf(platform).x, 2.0f, 0.05f);
        EXPECT_FLOAT_EQ(PositionOf(platform).y, 0.0f);
    }

    TEST_F(Physics2DTest, MovingADynamicBodysTransformTeleportsIt)
    {
        auto go = Make("Player", 0.0f, 0.0f);
        MakeBody(go, BodyType2D::Dynamic, 0.0f);
        go->AddComponent<CircleCollider2D>();
        Step();
        go->GetPositionable()->SetPosition(Vector3(30.0f, -4.0f, 0.0f));
        Step();
        EXPECT_NEAR(PositionOf(go).x, 30.0f, 0.001f);
        EXPECT_NEAR(PositionOf(go).y, -4.0f, 0.001f);
    }

    TEST_F(Physics2DTest, SurfaceSpeedMovesWhatTouchesIt)
    {
        auto belt = MakeFloor();
        belt->GetComponent<BoxCollider2D>()->SetSurfaceSpeed(2.0f);
        auto crate = Make("Crate", 0.0f, 0.5f);
        auto *rb = MakeBody(crate, BodyType2D::Dynamic);
        crate->AddComponent<BoxCollider2D>();
        Step(100);
        // The direction is documented in docs/physics2d.html from this: on a top surface, a positive speed moves things in +x
        EXPECT_GT(rb->GetLinearVelocity().x, 0.5f);
    }

    TEST_F(Physics2DTest, DisablingTakesABodyOutAndEnablingBringsItBackAtTheObjectsPose)
    {
        auto go = Make("Body", 0.0f, 0.0f);
        auto *rb = MakeBody(go, BodyType2D::Dynamic);
        go->AddComponent<CircleCollider2D>();
        Step();

        rb->SetActive(false);
        const float heldY = PositionOf(go).y;
        Step(20);
        EXPECT_FLOAT_EQ(PositionOf(go).y, heldY);
        RaycastHit2D hit;
        EXPECT_FALSE(Raycast(Vector2(-5.0f, heldY), Vector2(1.0f, 0.0f), hit));

        go->GetPositionable()->SetPosition(Vector3(9.0f, 9.0f, 0.0f));
        rb->SetActive(true);
        Step();
        EXPECT_NEAR(PositionOf(go).x, 9.0f, 0.01f);
        EXPECT_TRUE(Raycast(Vector2(0.0f, PositionOf(go).y), Vector2(1.0f, 0.0f), hit));
    }

    TEST_F(Physics2DTest, RemovingTheRigidbodyLeavesAStaticCollider)
    {
        auto go = Make("Body", 0.0f, 0.0f);
        go->AddComponent<Rigidbody2D>();
        go->AddComponent<BoxCollider2D>();
        Step();
        EXPECT_TRUE(go->RemoveComponent<Rigidbody2D>());
        const float y = PositionOf(go).y;
        Step(20);
        EXPECT_FLOAT_EQ(PositionOf(go).y, y);
        RaycastHit2D hit;
        EXPECT_TRUE(Raycast(Vector2(-5.0f, y), Vector2(1.0f, 0.0f), hit));
    }

    // ===== Events =====

    TEST_F(Physics2DTest, CollisionEventsFireOnBothBodiesInOrderWithANormalFromTheOther)
    {
        auto floor = MakeFloor();
        auto *floorRecorder = floor->AddComponent<Recorder>();
        auto ball = Make("Ball", 0.0f, 1.0f);
        auto *rb = MakeBody(ball, BodyType2D::Dynamic);
        ball->AddComponent<CircleCollider2D>();
        auto *ballRecorder = ball->AddComponent<Recorder>();

        Step(100);

        EXPECT_EQ(ballRecorder->collisionEnter, 1);
        EXPECT_EQ(floorRecorder->collisionEnter, 1);
        EXPECT_GT(ballRecorder->collisionStay, 1);
        EXPECT_EQ(ballRecorder->collisionExit, 0);
        ASSERT_FALSE(ballRecorder->order.empty());
        EXPECT_EQ(ballRecorder->order.front(), "collisionEnter");
        EXPECT_EQ(ballRecorder->order.at(1), "collisionStay"); // Stay in the same step as Enter, as in 3D

        const Collision2D &seen = ballRecorder->last;
        EXPECT_EQ(seen.gameObject, ball.get());
        EXPECT_EQ(seen.rigidbody, rb);
        EXPECT_EQ(seen.otherGameObject, floor.get());
        EXPECT_EQ(seen.otherRigidbody, nullptr); // a collider alone
        EXPECT_EQ(seen.collider, ball->GetComponent<CircleCollider2D>());
        EXPECT_EQ(seen.otherCollider, floor->GetComponent<BoxCollider2D>());
        ASSERT_NE(seen.GetContact(), nullptr);
        EXPECT_NEAR(seen.GetContact()->normal.y, 1.0f, 0.01f); // from the floor to the ball
        EXPECT_NEAR(seen.GetContact()->point.y, 0.0f, 0.05f);
        ASSERT_NE(floorRecorder->last.GetContact(), nullptr);
        EXPECT_NEAR(floorRecorder->last.GetContact()->normal.y, -1.0f, 0.01f);

        rb->SetLinearVelocity(Vector2(0.0f, 20.0f));
        Step(5);
        EXPECT_EQ(ballRecorder->collisionExit, 1);
        EXPECT_EQ(floorRecorder->collisionExit, 1);
        EXPECT_EQ(ballRecorder->order.back(), "collisionExit");
    }

    TEST_F(Physics2DTest, ACompoundBodyEntersOnceAndExitsOnlyWhenFullyAway)
    {
        auto floor = MakeFloor();
        auto compound = Make("Compound", 0.0f, 0.6f);
        auto *rb = MakeBody(compound, BodyType2D::Dynamic);
        rb->SetFixedRotation(true);
        auto *left = compound->AddComponent<BoxCollider2D>();
        left->SetOffset(Vector2(-1.0f, 0.0f));
        auto *right = compound->AddComponent<BoxCollider2D>();
        right->SetOffset(Vector2(1.0f, 0.0f));
        auto *recorder = compound->AddComponent<Recorder>();

        Step(100);
        EXPECT_EQ(recorder->collisionEnter, 1);

        EXPECT_TRUE(compound->RemoveComponent(std::type_index(typeid(BoxCollider2D))));
        Step(3);
        EXPECT_EQ(recorder->collisionExit, 0) << "one collider still touches";
        EXPECT_GT(recorder->collisionStay, 0);
    }

    TEST_F(Physics2DTest, TriggerEventsFireWithNoCollisionResponse)
    {
        auto zone = Make("Zone", 0.0f, 0.0f);
        auto *zoneCollider = zone->AddComponent<BoxCollider2D>();
        zoneCollider->SetIsTrigger(true);
        auto *zoneRecorder = zone->AddComponent<Recorder>();

        auto mover = Make("Mover", -3.0f, 0.0f);
        auto *rb = MakeBody(mover, BodyType2D::Dynamic, 0.0f);
        mover->AddComponent<BoxCollider2D>();
        rb->SetLinearVelocity(Vector2(5.0f, 0.0f));
        auto *moverRecorder = mover->AddComponent<Recorder>();

        Step(100);

        EXPECT_EQ(moverRecorder->triggerEnter, 1);
        EXPECT_EQ(zoneRecorder->triggerEnter, 1);
        EXPECT_GT(moverRecorder->triggerStay, 0);
        EXPECT_EQ(moverRecorder->triggerExit, 1);
        EXPECT_EQ(moverRecorder->collisionEnter, 0);
        EXPECT_GT(PositionOf(mover).x, 5.0f) << "it passed through";
        EXPECT_EQ(moverRecorder->lastTrigger.otherGameObject, zone.get());
        EXPECT_EQ(moverRecorder->lastTrigger.otherCollider, zoneCollider);
    }

    TEST_F(Physics2DTest, AKinematicBodyRaisesTriggerEventsButNoContactWithAStaticBody)
    {
        auto zone = Make("Zone", 0.0f, 0.0f);
        zone->AddComponent<BoxCollider2D>()->SetIsTrigger(true);
        auto wall = Make("Wall", 0.0f, 5.0f);
        wall->AddComponent<BoxCollider2D>();

        auto low = Make("Low", -3.0f, 0.0f);
        auto *lowBody = MakeBody(low, BodyType2D::Kinematic);
        low->AddComponent<BoxCollider2D>();
        lowBody->SetLinearVelocity(Vector2(5.0f, 0.0f));
        auto *lowRecorder = low->AddComponent<Recorder>();

        auto high = Make("High", -3.0f, 5.0f);
        auto *highBody = MakeBody(high, BodyType2D::Kinematic);
        high->AddComponent<BoxCollider2D>();
        highBody->SetLinearVelocity(Vector2(5.0f, 0.0f));
        auto *highRecorder = high->AddComponent<Recorder>();

        Step(100);

        EXPECT_EQ(lowRecorder->triggerEnter, 1);
        EXPECT_EQ(highRecorder->collisionEnter, 0);
    }

    TEST_F(Physics2DTest, TwoTriggersRaiseNothing)
    {
        auto zone = Make("Zone", 0.0f, 0.0f);
        zone->AddComponent<BoxCollider2D>()->SetIsTrigger(true);
        auto *zoneRecorder = zone->AddComponent<Recorder>();
        auto mover = Make("Mover", -3.0f, 0.0f);
        auto *rb = MakeBody(mover, BodyType2D::Dynamic, 0.0f);
        mover->AddComponent<BoxCollider2D>()->SetIsTrigger(true);
        rb->SetLinearVelocity(Vector2(5.0f, 0.0f));
        auto *moverRecorder = mover->AddComponent<Recorder>();

        Step(100);

        EXPECT_EQ(moverRecorder->triggerEnter, 0);
        EXPECT_EQ(zoneRecorder->triggerEnter, 0);
        EXPECT_EQ(moverRecorder->triggerExit, 0);
    }

    TEST_F(Physics2DTest, TogglingATriggerEndsThePairAndStartsItAgain)
    {
        auto floor = MakeFloor();
        auto ball = Make("Ball", 0.0f, 0.5f);
        MakeBody(ball, BodyType2D::Dynamic);
        auto *collider = ball->AddComponent<CircleCollider2D>();
        auto *recorder = ball->AddComponent<Recorder>();
        Step(10);
        ASSERT_EQ(recorder->collisionEnter, 1);

        collider->SetIsTrigger(true);
        Step(2);
        EXPECT_EQ(recorder->collisionExit, 1);
        EXPECT_EQ(recorder->triggerEnter, 1);
    }

    TEST_F(Physics2DTest, ACollidersRemovalEndsItsPairWithAnExitBeforeANewEnter)
    {
        auto floor = MakeFloor();
        auto ball = Make("Ball", 0.0f, 0.5f);
        MakeBody(ball, BodyType2D::Dynamic);
        ball->AddComponent<CircleCollider2D>();
        auto *recorder = ball->AddComponent<Recorder>();
        Step(10);
        ASSERT_EQ(recorder->collisionEnter, 1);

        ball->RemoveComponent<CircleCollider2D>();
        ball->AddComponent<CircleCollider2D>();
        recorder->order.clear();
        Step(3);

        ASSERT_GE(recorder->order.size(), 2u);
        EXPECT_EQ(recorder->order.front(), "collisionExit");
        EXPECT_EQ(recorder->last.otherGameObject, floor.get());
    }

    // ===== Changing the world from inside a handler =====

    class DestroysOtherOnEnter final : public Component
    {
    public:
        explicit DestroysOtherOnEnter(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "DestroysOther2D"; }
        void OnCollisionEnter2D(const Collision2D &c) override
        {
            ++entered;
            if (c.otherGameObject)
            {
                c.otherGameObject->Destroy();
            }
        }
        int entered = 0;
    };

    class RemovesOwnBodyOnEnter final : public Component
    {
    public:
        explicit RemovesOwnBodyOnEnter(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "RemovesBody2D"; }
        void OnCollisionEnter2D(const Collision2D &) override
        {
            GetGameObject().RemoveComponent<CircleCollider2D>();
            GetGameObject().RemoveComponent<Rigidbody2D>();
        }
    };

    class SpawnsOnEnter final : public Component
    {
    public:
        explicit SpawnsOnEnter(GameObject &gameObject) : Component(gameObject) {}
        [[nodiscard]] std::string GetTypeName() const override { return "Spawns2D"; }
        void OnCollisionEnter2D(const Collision2D &) override
        {
            auto spawned = GameObject::Create("Spawned");
            spawned->CreatePositionable();
            GetGameObject().GetScene()->AddRootGameObject(spawned);
            auto *rb = spawned->AddComponent<Rigidbody2D>();
            spawned->AddComponent<CircleCollider2D>();
            rb->SetGravityScale(0.0f);
            rb->SetLinearVelocity(Vector2(1.0f, 0.0f));
            ++spawnedCount;
        }
        int spawnedCount = 0;
    };

    TEST_F(Physics2DTest, DestroyingTheOtherObjectInAHandlerIsSafe)
    {
        auto floor = MakeFloor();
        auto ball = Make("Ball", 0.0f, 1.0f);
        MakeBody(ball, BodyType2D::Dynamic);
        ball->AddComponent<CircleCollider2D>();
        auto *destroyer = ball->AddComponent<DestroysOtherOnEnter>();

        Step(60);
        EXPECT_EQ(destroyer->entered, 1);
        EXPECT_LT(PositionOf(ball).y, -0.5f) << "the floor went away, so it kept falling";
    }

    TEST_F(Physics2DTest, RemovingABodyInsideItsOwnHandlerIsSafeAndTheOtherSideGetsItsExit)
    {
        auto floor = MakeFloor();
        auto *floorRecorder = floor->AddComponent<Recorder>();
        auto ball = Make("Ball", 0.0f, 1.0f);
        MakeBody(ball, BodyType2D::Dynamic);
        ball->AddComponent<CircleCollider2D>();
        ball->AddComponent<RemovesOwnBodyOnEnter>();

        Step(60);
        EXPECT_EQ(floorRecorder->collisionEnter, 1);
        EXPECT_EQ(floorRecorder->collisionExit, 1);
    }

    TEST_F(Physics2DTest, CreatingABodyInAHandlerIsSafeAndItSimulates)
    {
        MakeFloor();
        auto ball = Make("Ball", 0.0f, 1.0f);
        MakeBody(ball, BodyType2D::Dynamic);
        ball->AddComponent<CircleCollider2D>();
        auto *spawner = ball->AddComponent<SpawnsOnEnter>();

        Step(60);
        ASSERT_EQ(spawner->spawnedCount, 1);
        const auto spawned = _scene->FindGameObject("Spawned");
        ASSERT_NE(spawned, nullptr);
        EXPECT_GT(PositionOf(spawned).x, 0.1f);
    }

    // ===== Layers =====

    TEST_F(Physics2DTest, TheSharedMatrixFiltersSolidsAndTriggers)
    {
        constexpr int kA = 8, kB = 9;
        Layers::SetCollision(kA, kB, false);

        auto wall = Make("Wall", 0.0f, 0.0f);
        wall->SetLayer(kB);
        wall->AddComponent<BoxCollider2D>();
        auto zone = Make("Zone", 0.0f, 3.0f);
        zone->SetLayer(kB);
        zone->AddComponent<BoxCollider2D>()->SetIsTrigger(true);

        auto mover = Make("Mover", -3.0f, 0.0f);
        mover->SetLayer(kA);
        auto *rb = MakeBody(mover, BodyType2D::Dynamic, 0.0f);
        mover->AddComponent<BoxCollider2D>();
        rb->SetLinearVelocity(Vector2(5.0f, 0.0f));
        auto *recorder = mover->AddComponent<Recorder>();
        auto high = Make("High", -3.0f, 3.0f);
        high->SetLayer(kA);
        auto *highBody = MakeBody(high, BodyType2D::Dynamic, 0.0f);
        high->AddComponent<BoxCollider2D>();
        highBody->SetLinearVelocity(Vector2(5.0f, 0.0f));
        auto *highRecorder = high->AddComponent<Recorder>();

        Step(100);

        EXPECT_EQ(recorder->collisionEnter, 0);
        EXPECT_GT(PositionOf(mover).x, 5.0f);
        EXPECT_EQ(highRecorder->triggerEnter, 0);
    }

    TEST_F(Physics2DTest, AMatrixChangeWhileTouchingEndsThePairAndChangingItBackStartsIt)
    {
        constexpr int kA = 8, kB = 9;
        auto floor = MakeFloor();
        floor->SetLayer(kB);
        auto ball = Make("Ball", 0.0f, 0.5f);
        ball->SetLayer(kA);
        auto *rb = MakeBody(ball, BodyType2D::Dynamic, 0.0f);
        ball->AddComponent<CircleCollider2D>();
        auto *recorder = ball->AddComponent<Recorder>();
        Step(5);
        ASSERT_EQ(recorder->collisionEnter, 1);
        (void)rb;

        Layers::SetCollision(kA, kB, false);
        Step(2);
        EXPECT_EQ(recorder->collisionExit, 1);

        Layers::SetCollision(kA, kB, true);
        Step(2);
        EXPECT_EQ(recorder->collisionEnter, 2);
    }

    TEST_F(Physics2DTest, SetLayerMovesTheCollider)
    {
        constexpr int kA = 8, kB = 9;
        Layers::SetCollision(kA, kB, false);
        auto floor = MakeFloor();
        floor->SetLayer(kB);
        auto ball = Make("Ball", 0.0f, 0.5f);
        ball->SetLayer(kA);
        MakeBody(ball, BodyType2D::Dynamic);
        ball->AddComponent<CircleCollider2D>();
        auto *recorder = ball->AddComponent<Recorder>();
        Step(3);
        EXPECT_EQ(recorder->collisionEnter, 0);

        ball->SetLayer(kB);
        ball->GetPositionable()->SetPosition(Vector3(0.0f, 0.5f, 0.0f));
        Step(3);
        EXPECT_EQ(recorder->collisionEnter, 1);
    }

    // ===== Queries, against Physics::QueryRules =====

    TEST_F(Physics2DTest, RaycastFindsTheNearestWithDistancePointAndNormal)
    {
        auto near = Make("Near", 5.0f, 0.0f);
        near->AddComponent<BoxCollider2D>();
        auto far = Make("Far", 10.0f, 0.0f);
        far->AddComponent<BoxCollider2D>();
        Step();

        RaycastHit2D hit;
        ASSERT_TRUE(Raycast(Vector2(0.0f, 0.0f), Vector2(2.0f, 0.0f), hit)); // not unit length
        EXPECT_TRUE(hit.hit);
        EXPECT_EQ(hit.gameObject, near.get());
        EXPECT_EQ(hit.collider, near->GetComponent<BoxCollider2D>());
        EXPECT_NEAR(hit.distance, 4.5f, 0.001f);
        EXPECT_NEAR(hit.point.x, 4.5f, 0.001f);
        EXPECT_NEAR(hit.normal.x, -1.0f, 0.001f);

        EXPECT_FALSE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit, 2.0f)) << "maxDistance is respected";
    }

    TEST_F(Physics2DTest, DegenerateQueriesFindNothing)
    {
        Make("Box", 0.0f, 0.0f)->AddComponent<BoxCollider2D>();
        Step();

        RaycastHit2D hit;
        std::vector<OverlapHit2D> overlaps;
        const float nan = std::nanf("");
        EXPECT_FALSE(Raycast(Vector2(-5.0f, 0.0f), Vector2(0.0f, 0.0f), hit));
        EXPECT_FALSE(Raycast(Vector2(nan, 0.0f), Vector2(1.0f, 0.0f), hit));
        EXPECT_FALSE(Raycast(Vector2(-5.0f, 0.0f), Vector2(1.0f, 0.0f), hit, 0.0f));
        EXPECT_FALSE(Raycast(Vector2(-5.0f, 0.0f), Vector2(1.0f, 0.0f), hit, nan));
        EXPECT_FALSE(Raycast(Vector2(-5.0f, 0.0f), Vector2(1.0f, 0.0f), hit, kNoLimit, 0u));
        EXPECT_FALSE(CircleCast(Vector2(-5.0f, 0.0f), 0.0f, Vector2(1.0f, 0.0f), hit));
        EXPECT_FALSE(BoxCast(Vector2(-5.0f, 0.0f), Vector2(0.0f, 1.0f), 0.0f, Vector2(1.0f, 0.0f), hit));
        EXPECT_EQ(OverlapCircle(Vector2(0.0f, 0.0f), -1.0f, overlaps), 0);
        EXPECT_EQ(OverlapBox(Vector2(0.0f, 0.0f), Vector2(-1.0f, 1.0f), 0.0f, overlaps), 0);
        EXPECT_EQ(OverlapCircle(Vector2(0.0f, 0.0f), 10.0f, overlaps, 0u), 0);
    }

    TEST_F(Physics2DTest, AnInfiniteRayIsClampedAndStillHits)
    {
        Make("Box", 100.0f, 0.0f)->AddComponent<BoxCollider2D>();
        Step();
        RaycastHit2D hit;
        ASSERT_TRUE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit));
        EXPECT_NEAR(hit.distance, 99.5f, 0.1f);
    }

    TEST_F(Physics2DTest, ARayStartingInsideHitsAtDistanceZeroAgainstItsDirection)
    {
        auto box = Make("Box", 0.0f, 0.0f);
        box->AddComponent<BoxCollider2D>();
        auto circle = Make("Circle", 20.0f, 0.0f);
        circle->AddComponent<CircleCollider2D>();
        Step();

        for (const Vector2 start : {Vector2(0.1f, 0.1f), Vector2(20.1f, 0.1f)})
        {
            RaycastHit2D hit;
            ASSERT_TRUE(Raycast(start, Vector2(0.0f, 1.0f), hit, 0.2f));
            EXPECT_FLOAT_EQ(hit.distance, 0.0f);
            EXPECT_NEAR(hit.normal.y, -1.0f, 0.001f);
            EXPECT_FLOAT_EQ(hit.point.x, start.x);
            EXPECT_FLOAT_EQ(hit.point.y, start.y);

            std::vector<RaycastHit2D> all;
            EXPECT_EQ(RaycastAll(start, Vector2(0.0f, 1.0f), all, 0.2f), 1) << "reported once";
        }
    }

    TEST_F(Physics2DTest, ACastStartingInsideHitsAtDistanceZero)
    {
        Make("Box", 0.0f, 0.0f)->AddComponent<BoxCollider2D>();
        Step();
        RaycastHit2D hit;
        ASSERT_TRUE(CircleCast(Vector2(0.0f, 0.0f), 0.1f, Vector2(1.0f, 0.0f), hit, 5.0f));
        EXPECT_FLOAT_EQ(hit.distance, 0.0f);
        EXPECT_NEAR(hit.normal.x, -1.0f, 0.001f);
        EXPECT_FLOAT_EQ(hit.point.x, 0.0f);
    }

    TEST_F(Physics2DTest, RaycastAllIsNearestFirstAndTiesGoByBodyHandle)
    {
        auto a = Make("A", 5.0f, 0.0f);
        a->AddComponent<BoxCollider2D>();
        auto b = Make("B", 10.0f, 0.0f);
        b->AddComponent<BoxCollider2D>();
        // Two bodies with the same near face, created in this order
        auto tieFirst = Make("TieFirst", 20.0f, 3.0f);
        auto *firstCollider = tieFirst->AddComponent<BoxCollider2D>();
        auto tieSecond = Make("TieSecond", 20.0f, 3.0f);
        auto *secondCollider = tieSecond->AddComponent<BoxCollider2D>();
        Step();

        std::vector<RaycastHit2D> hits;
        ASSERT_EQ(RaycastAll(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hits), 2);
        EXPECT_EQ(hits[0].gameObject, a.get());
        EXPECT_EQ(hits[1].gameObject, b.get());

        ASSERT_EQ(RaycastAll(Vector2(0.0f, 3.0f), Vector2(1.0f, 0.0f), hits), 2);
        EXPECT_EQ(hits[0].collider, firstCollider);
        EXPECT_EQ(hits[1].collider, secondCollider);
        RaycastHit2D single;
        ASSERT_TRUE(Raycast(Vector2(0.0f, 3.0f), Vector2(1.0f, 0.0f), single));
        EXPECT_EQ(single.collider, firstCollider);
    }

    TEST_F(Physics2DTest, TriggersAreHitUnlessIgnored)
    {
        auto zone = Make("Zone", 5.0f, 0.0f);
        zone->AddComponent<BoxCollider2D>()->SetIsTrigger(true);
        Step();

        RaycastHit2D hit;
        std::vector<OverlapHit2D> overlaps;
        EXPECT_TRUE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit));
        EXPECT_FALSE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit, kNoLimit, Layers::DefaultRaycastMask, Physics::QueryTriggers::Ignore));
        EXPECT_EQ(OverlapCircle(Vector2(5.0f, 0.0f), 1.0f, overlaps), 1);
        EXPECT_EQ(OverlapCircle(Vector2(5.0f, 0.0f), 1.0f, overlaps, Layers::DefaultRaycastMask, Physics::QueryTriggers::Ignore), 0);
        EXPECT_FALSE(CircleCast(Vector2(0.0f, 0.0f), 0.1f, Vector2(1.0f, 0.0f), hit, kNoLimit, Layers::DefaultRaycastMask, Physics::QueryTriggers::Ignore));
    }

    TEST_F(Physics2DTest, TheLayerMaskPicksLayersAndTheMatrixDoesNotChangeQueries)
    {
        constexpr int kA = 8, kB = 9;
        auto a = Make("A", 5.0f, 0.0f);
        a->SetLayer(kA);
        a->AddComponent<BoxCollider2D>();
        auto b = Make("B", 10.0f, 0.0f);
        b->SetLayer(kB);
        b->AddComponent<BoxCollider2D>();
        Step();

        // The layer collides with nothing, and queries must still see it
        for (int other = 0; other < Layers::Count; ++other)
        {
            Layers::SetCollision(kA, other, false);
        }

        RaycastHit2D hit;
        ASSERT_TRUE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit, kNoLimit, Layers::MaskOf(kA)));
        EXPECT_EQ(hit.gameObject, a.get());
        ASSERT_TRUE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit, kNoLimit, Layers::MaskOf(kB)));
        EXPECT_EQ(hit.gameObject, b.get());
    }

    TEST_F(Physics2DTest, OverlapsAreSortedByBodyThenColliderOrder)
    {
        auto first = Make("First", 0.0f, 0.0f);
        auto *a = first->AddComponent<BoxCollider2D>();
        auto second = Make("Second", 1.0f, 0.0f);
        auto *rb = MakeBody(second, BodyType2D::Static);
        auto *c0 = second->AddComponent<CircleCollider2D>();
        auto *c1 = second->AddComponent<BoxCollider2D>();
        Step();

        std::vector<OverlapHit2D> hits;
        ASSERT_EQ(OverlapCircle(Vector2(0.5f, 0.0f), 3.0f, hits), 3);
        EXPECT_EQ(hits[0].collider, a);
        EXPECT_EQ(hits[0].rigidbody, nullptr);
        EXPECT_EQ(hits[1].collider, c0);
        EXPECT_EQ(hits[1].rigidbody, rb);
        EXPECT_EQ(hits[2].collider, c1);

        ASSERT_EQ(OverlapBox(Vector2(0.0f, 0.0f), Vector2(0.1f, 0.1f), 0.0f, hits), 1);
        EXPECT_EQ(hits[0].collider, a);
        EXPECT_EQ(OverlapBox(Vector2(50.0f, 0.0f), Vector2(1.0f, 1.0f), 0.5f, hits), 0);
    }

    TEST_F(Physics2DTest, ShapeCastsReportTheNearestHit)
    {
        auto wall = Make("Wall", 5.0f, 0.0f);
        wall->AddComponent<BoxCollider2D>();
        Step();

        RaycastHit2D hit;
        ASSERT_TRUE(CircleCast(Vector2(0.0f, 0.0f), 0.5f, Vector2(1.0f, 0.0f), hit));
        EXPECT_NEAR(hit.distance, 4.0f, 0.01f);
        EXPECT_NEAR(hit.normal.x, -1.0f, 0.01f);
        EXPECT_EQ(hit.gameObject, wall.get());

        ASSERT_TRUE(BoxCast(Vector2(0.0f, 0.0f), Vector2(1.0f, 1.0f), 0.0f, Vector2(1.0f, 0.0f), hit));
        EXPECT_NEAR(hit.distance, 3.5f, 0.01f);
    }

    TEST(Physics2DNoSceneTest, QueriesWithNoWorldFindNothing)
    {
        SceneManager::AddScene(Scene::Create("Physics2D_Empty"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        RaycastHit2D hit;
        std::vector<OverlapHit2D> overlaps;
        EXPECT_FALSE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit));
        EXPECT_EQ(OverlapCircle(Vector2(0.0f, 0.0f), 1.0f, overlaps), 0);
    }

    // ===== Scenes =====

    TEST_F(Physics2DTest, ASceneSwitchFreesTheWorldAndALoadIsClean)
    {
        Make("Body", 0.0f, 0.0f)->AddComponent<CircleCollider2D>();
        Step();
        ASSERT_NE(_scene->GetPhysics2DWorld(), nullptr);

        SceneManager::AddScene(Scene::Create("Physics2D_Next"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
        EXPECT_EQ(_scene->GetPhysics2DWorld(), nullptr);

        RaycastHit2D hit;
        EXPECT_FALSE(Raycast(Vector2(-5.0f, 0.0f), Vector2(1.0f, 0.0f), hit));
    }

    TEST_F(Physics2DTest, TwoScenesWorldsDoNotMeet)
    {
        auto parked = Make("ParkedWall", 5.0f, 0.0f);
        parked->AddComponent<BoxCollider2D>();
        Step();

        auto stage = SceneManager::SwapLoadedScene(Scene::Create("Physics2D_Stage"));
        Scene *stageScene = SceneManager::GetCurScene();
        ASSERT_NE(stageScene, _scene);
        RaycastHit2D hit;
        EXPECT_FALSE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit));

        // The parked scene keeps its world and its bodies
        auto restored = SceneManager::SwapLoadedScene(std::move(stage));
        EXPECT_EQ(restored.get(), stageScene);
        EXPECT_TRUE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit));
    }

    TEST_F(Physics2DTest, ALayerChangeOnAParkedWorldDoesNotCrash)
    {
        Make("Body", 0.0f, 0.0f)->AddComponent<CircleCollider2D>();
        Step();
        auto stage = SceneManager::SwapLoadedScene(Scene::Create("Physics2D_Stage"));
        Layers::SetCollision(0, 1, false);
        auto restored = SceneManager::SwapLoadedScene(std::move(stage));
        Step();
    }

    // ===== Determinism =====

    std::uint64_t RunCrates(Scene &scene, const int steps)
    {
        auto floor = GameObject::Create("Floor");
        floor->CreatePositionable();
        floor->GetPositionable()->SetPosition(Vector3(0.0f, -1.0f, 0.0f));
        scene.AddRootGameObject(floor);
        floor->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(20.0f, 1.0f));
        for (int i = 0; i < 24; ++i)
        {
            auto crate = GameObject::Create("Crate" + std::to_string(i));
            crate->CreatePositionable();
            crate->GetPositionable()->SetPosition(Vector3(static_cast<float>(i % 6) * 0.4f - 1.0f, 1.0f + static_cast<float>(i) * 0.9f, 0.0f));
            scene.AddRootGameObject(crate);
            auto *rb = crate->AddComponent<Rigidbody2D>();
            if (i % 2 == 0)
            {
                crate->AddComponent<BoxCollider2D>()->SetHalfExtents(Vector2(0.3f, 0.3f));
            }
            else
            {
                crate->AddComponent<CircleCollider2D>()->SetRadius(0.3f);
            }
            rb->SetAngularVelocity(static_cast<float>(i) * 0.1f);
        }
        for (int i = 0; i < steps; ++i)
        {
            scene.ProcessAttachQueue();
            EXPECT_EQ(Application::GetInstance().StepTicks(1), 1u);
        }
        return scene.GetPhysics2DWorld() ? scene.GetPhysics2DWorld()->HashState() : 0;
    }

    TEST_F(Physics2DTest, TheSameInputsGiveTheSameHashOnTwoRunsAndEveryLeg)
    {
        const std::uint64_t first = RunCrates(*_scene, 120);

        SceneManager::AddScene(Scene::Create("Physics2D_SecondRun"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        _scene = SceneManager::GetCurScene();
        const std::uint64_t second = RunCrates(*_scene, 120);

        // Measured in CI on every leg. A leg that differs is a finding to report; do not loosen this to make it pass.
        constexpr std::uint64_t kGolden = 0x0000000000000000ull;
        std::printf("[PHYSICS2D-DETERMINISM] steps=120 hash=0x%016llx golden=0x%016llx\n", static_cast<unsigned long long>(first), static_cast<unsigned long long>(kGolden));
        EXPECT_EQ(first, second);
        EXPECT_NE(first, 0u);
        EXPECT_EQ(first, kGolden);
    }

    // ===== Serialization =====

    TEST(Physics2DSerializationTest, ComponentsAndTheirSettingsSurviveASceneLoad)
    {
        const auto go = GameObject::Create("Body");
        go->CreatePositionable();
        auto *rb = go->AddComponent<Rigidbody2D>();
        rb->SetBodyType(BodyType2D::Kinematic);
        rb->SetMass(4.0f);
        rb->SetLinearDamping(0.25f);
        rb->SetAngularDamping(0.5f);
        rb->SetGravityScale(0.75f);
        rb->SetFixedRotation(true);
        rb->SetBullet(true);
        auto *box = go->AddComponent<BoxCollider2D>();
        box->SetHalfExtents(Vector2(2.0f, 3.0f));
        box->SetOffset(Vector2(0.5f, -0.5f));
        box->SetIsTrigger(true);
        box->SetDensity(2.5f);
        box->SetSurfaceSpeed(1.5f);
        go->AddComponent<CircleCollider2D>()->SetRadius(4.0f);
        go->AddComponent<PolygonCollider2D>()->SetVertices({{0.0f, 0.0f}, {2.0f, 0.0f}, {0.0f, 2.0f}});

        const auto scene = Scene::Create("Physics2D_RoundTrip");
        scene->AddRootGameObject(go);
        const auto loaded = Scene::FromJSON(scene->Serialize());
        ASSERT_NE(loaded, nullptr);
        const auto back = loaded->FindGameObject("Body");
        ASSERT_NE(back, nullptr);

        const auto *rbBack = back->GetComponent<Rigidbody2D>();
        ASSERT_NE(rbBack, nullptr);
        EXPECT_EQ(rbBack->GetBodyType(), BodyType2D::Kinematic);
        EXPECT_FALSE(rbBack->UsesAutoMass());
        EXPECT_FLOAT_EQ(rbBack->GetMass(), 4.0f);
        EXPECT_FLOAT_EQ(rbBack->GetLinearDamping(), 0.25f);
        EXPECT_FLOAT_EQ(rbBack->GetAngularDamping(), 0.5f);
        EXPECT_FLOAT_EQ(rbBack->GetGravityScale(), 0.75f);
        EXPECT_TRUE(rbBack->IsFixedRotation());
        EXPECT_TRUE(rbBack->IsBullet());

        const auto *boxBack = back->GetComponent<BoxCollider2D>();
        ASSERT_NE(boxBack, nullptr);
        EXPECT_FLOAT_EQ(boxBack->GetHalfExtents().y, 3.0f);
        EXPECT_FLOAT_EQ(boxBack->GetOffset().x, 0.5f);
        EXPECT_TRUE(boxBack->IsTrigger());
        EXPECT_FLOAT_EQ(boxBack->GetDensity(), 2.5f);
        EXPECT_FLOAT_EQ(boxBack->GetSurfaceSpeed(), 1.5f);

        EXPECT_FLOAT_EQ(back->GetComponent<CircleCollider2D>()->GetRadius(), 4.0f);
        const auto &vertices = back->GetComponent<PolygonCollider2D>()->GetVertices();
        ASSERT_EQ(vertices.size(), 3u);
        EXPECT_FLOAT_EQ(vertices[1].x, 2.0f);
    }

    TEST_F(Physics2DTest, APrefabInstanceSimulatesAndKeepsAChangedFieldThroughASave)
    {
        const auto source = GameObject::Create("Crate");
        source->CreatePositionable();
        source->AddComponent<Rigidbody2D>();
        source->AddComponent<BoxCollider2D>()->SetDensity(2.0f);

        const auto instance = PrefabManager::InstantiatePrefab(source->Serialize());
        ASSERT_NE(instance, nullptr);
        _scene->AddRootGameObject(instance);
        instance->GetComponent<BoxCollider2D>()->SetDensity(5.0f); // this instance differs from the prefab
        instance->GetPositionable()->SetPosition(Vector3(0.0f, 5.0f, 0.0f));
        Step(5);
        EXPECT_LT(PositionOf(instance).y, 5.0f);
        EXPECT_NEAR(instance->GetComponent<Rigidbody2D>()->GetMass(), 5.0f, 0.01f);

        const auto loaded = Scene::FromJSON(_scene->Serialize());
        ASSERT_NE(loaded, nullptr);
        const auto back = loaded->FindGameObject("Crate");
        ASSERT_NE(back, nullptr);
        EXPECT_FLOAT_EQ(back->GetComponent<BoxCollider2D>()->GetDensity(), 5.0f);
    }

    TEST(Physics2DRegistryTest, TheFourTypesAreRegistered)
    {
        auto &registry = ComponentRegistry::Instance();
        for (const char *type : {"Rigidbody2D", "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D"})
        {
            EXPECT_TRUE(registry.Describe(type).has_value()) << type;
        }
    }

#else // A build without Box2D

    using Physics2DStrippedTest = Physics2DSceneTest;

    TEST_F(Physics2DStrippedTest, ComponentsDoNothingAndKeepTheirData)
    {
        auto go = Make("Body", 0.0f, 3.0f);
        auto *rb = go->AddComponent<Rigidbody2D>();
        rb->SetMass(4.0f);
        rb->SetLinearVelocity(Vector2(1.0f, 0.0f));
        rb->AddForce(Vector2(1.0f, 0.0f));
        auto *box = go->AddComponent<BoxCollider2D>();
        box->SetHalfExtents(Vector2(2.0f, 3.0f));
        box->SetIsTrigger(true);
        go->AddComponent<PolygonCollider2D>();

        Step(10);

        EXPECT_EQ(_scene->GetPhysics2DWorld(), nullptr);
        EXPECT_FLOAT_EQ(PositionOf(go).y, 3.0f);
        EXPECT_FLOAT_EQ(rb->GetMass(), 4.0f);

        const auto loaded = Scene::FromJSON(_scene->Serialize());
        ASSERT_NE(loaded, nullptr);
        const auto back = loaded->FindGameObject("Body");
        ASSERT_NE(back, nullptr);
        ASSERT_NE(back->GetComponent<BoxCollider2D>(), nullptr);
        EXPECT_FLOAT_EQ(back->GetComponent<BoxCollider2D>()->GetHalfExtents().y, 3.0f);
    }

    TEST_F(Physics2DStrippedTest, QueriesFindNothing)
    {
        RaycastHit2D hit;
        std::vector<OverlapHit2D> overlaps;
        EXPECT_FALSE(Raycast(Vector2(0.0f, 0.0f), Vector2(1.0f, 0.0f), hit));
        EXPECT_FALSE(CircleCast(Vector2(0.0f, 0.0f), 1.0f, Vector2(1.0f, 0.0f), hit));
        EXPECT_EQ(OverlapCircle(Vector2(0.0f, 0.0f), 1.0f, overlaps), 0);
    }

    TEST_F(Physics2DStrippedTest, TheRegistryIsTheSameAsAFullBuilds)
    {
        auto &registry = ComponentRegistry::Instance();
        for (const char *type : {"Rigidbody2D", "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D"})
        {
            EXPECT_TRUE(registry.Describe(type).has_value()) << type;
        }
    }

#endif
}
