// Layers in a real physics scene, as TYPED tests over every backend this build has (PhysX and Jolt): the collision
// matrix for contacts and triggers, runtime layer and matrix changes (also next to a sleeping body and while a body is
// disabled), and, on PhysX only until stage J5, the query masks. A difference between the backends that a test has to
// allow for is written next to the assertion and listed in docs/physics.html ("Jolt backend", differences).
//
// Query tests: the Jolt backend has no scene queries before stage J5 (its Raycast answers "nothing found"), so those
// tests end in GTEST_SKIP on Jolt with the reason, and show as skipped in the output. J5 removes the skips.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <string>
#include <vector>

#include "engine/Component.hpp"
#include "engine/Layers.hpp"
#include "engine/physics/Raycast.hpp"

namespace N2PhysicsTests
{
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

    // The scene fixture of the core tests, with the layer registry reset around every test
    template <typename Tag>
    class LayerPhysicsTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        void SetUp() override
        {
            Layers::ResetToDefaults();
            PhysicsCoreFixture<Tag>::SetUp();
        }

        void TearDown() override
        {
            PhysicsCoreFixture<Tag>::TearDown();
            Layers::ResetToDefaults();
        }

        GameObject::Ptr SpawnOnLayer(const std::string &name, const Vector3 &position, const int layer)
        {
            const auto go = this->Spawn(name, position);
            go->SetLayer(layer);
            return go;
        }

        GameObject::Ptr SpawnBox(const std::string &name, const Vector3 &position, const int layer)
        {
            const auto go = SpawnOnLayer(name, position, layer);
            go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
            return go;
        }

        // Static 10x1x10 floor whose top surface is at y = 0.5
        GameObject::Ptr SpawnFloorOn(const int layer)
        {
            const auto floor = SpawnOnLayer("Floor", Vector3(0.0f, 0.0f, 0.0f), layer);
            floor->template AddComponent<BoxCollider>()->SetSize(Vector3(10.0f, 1.0f, 10.0f));
            return floor;
        }

        GameObject::Ptr SpawnBallOn(const std::string &name, const Vector3 &position, const int layer)
        {
            const auto ball = SpawnOnLayer(name, position, layer);
            ball->template AddComponent<SphereCollider>()->SetRadius(0.5f);
            auto *body = ball->template AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(true);
            return ball;
        }

        // A 4x4x4 trigger volume at the origin
        GameObject::Ptr SpawnZone(const int layer)
        {
            const auto zone = SpawnOnLayer("Zone", Vector3(0.0f, 0.0f, 0.0f), layer);
            auto *collider = zone->template AddComponent<BoxCollider>();
            collider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
            collider->SetIsTrigger(true);
            return zone;
        }

        // A weightless dynamic sphere inside the zone (it stays put and keeps overlapping)
        GameObject::Ptr SpawnVisitor(const int layer)
        {
            const auto visitor = SpawnOnLayer("Visitor", Vector3(0.0f, 0.0f, 0.0f), layer);
            visitor->template AddComponent<SphereCollider>()->SetRadius(0.5f);
            auto *body = visitor->template AddComponent<Rigidbody>();
            body->SetBodyType(BodyType::Dynamic);
            body->SetGravityEnabled(false);
            return visitor;
        }
    };

    TYPED_TEST_SUITE(LayerPhysicsTest, AllBackends, BackendNames);

// The Jolt backend has no scene queries before stage J5: say so, visibly, and stop
#define N2_SKIP_QUERIES_ON_JOLT()                                                                                    \
    if constexpr (TypeParam::kIsJolt)                                                                                \
    {                                                                                                                \
        GTEST_SKIP() << "scene queries on Jolt arrive in stage J5 (JoltBackend::Raycast answers 'nothing found')"; \
    }

    // ============================================================================
    // Queries
    // ============================================================================

    TYPED_TEST(LayerPhysicsTest, QueryMaskIncludesAndExcludesLayers)
    {
        N2_SKIP_QUERIES_ON_JOLT();
        const auto nearBox = this->SpawnBox("Near", Vector3(0.0f, 0.0f, 5.0f), LayerA);
        const auto farBox = this->SpawnBox("Far", Vector3(0.0f, 0.0f, 10.0f), LayerB);
        this->Step(1);

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

    TYPED_TEST(LayerPhysicsTest, IgnoreRaycastIsSkippedByDefault)
    {
        N2_SKIP_QUERIES_ON_JOLT();
        const auto ignored = this->SpawnBox("Ignored", Vector3(0.0f, 0.0f, 5.0f), Layers::IgnoreRaycast);
        const auto target = this->SpawnBox("Target", Vector3(0.0f, 0.0f, 10.0f), Layers::Default);
        this->Step(1);

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

    TYPED_TEST(LayerPhysicsTest, MaskZeroHitsNothing)
    {
        N2_SKIP_QUERIES_ON_JOLT();
        this->SpawnBox("Target", Vector3(0.0f, 0.0f, 5.0f), Layers::Default);
        this->Step(1);

        // An all-zero mask used to switch PhysX's layer test off and hit everything
        RaycastHit hit;
        EXPECT_FALSE(Raycast::Single(Vector3::Zero, AlongZ, hit, 100.0f, 0u));
        EXPECT_FALSE(hit.hit);
        std::vector<RaycastHit> hits;
        EXPECT_EQ(Raycast::All(Vector3::Zero, AlongZ, hits, 100.0f, 0u), 0);
        EXPECT_FALSE(Raycast::SphereCast(Vector3::Zero, 0.1f, AlongZ, hit, 100.0f, 0u));
        EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, 0u));
    }

    TYPED_TEST(LayerPhysicsTest, ChangingAnObjectsLayerMovesItsQueryLayerAtOnce)
    {
        N2_SKIP_QUERIES_ON_JOLT();
        const auto box = this->SpawnBox("Box", Vector3(0.0f, 0.0f, 5.0f), Layers::Default);
        this->Step(1);
        ASSERT_TRUE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f));

        box->SetLayer(Layers::IgnoreRaycast);
        EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f));
        EXPECT_TRUE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(Layers::IgnoreRaycast)));

        box->SetLayer(LayerA);
        EXPECT_TRUE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(LayerA)));
        EXPECT_FALSE(Raycast::Any(Vector3::Zero, AlongZ, 100.0f, Layers::MaskOf(Layers::IgnoreRaycast)));
    }

    TYPED_TEST(LayerPhysicsTest, ChildColliderUsesItsOwnLayer)
    {
        N2_SKIP_QUERIES_ON_JOLT();
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

        this->_scene->AddRootGameObject(parent);
        this->Step(1);

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

    TYPED_TEST(LayerPhysicsTest, NonCollidingLayersPassThroughEachOther)
    {
        Layers::SetCollision(LayerA, LayerB, false);
        const auto floor = this->SpawnFloorOn(LayerA);
        auto *floorEvents = floor->template AddComponent<LayerEventCounter>();
        const auto ball = this->SpawnBallOn("Ghost", Vector3(0.0f, 2.0f, 0.0f), LayerB);
        auto *ballEvents = ball->template AddComponent<LayerEventCounter>();

        this->Step(90);

        EXPECT_LT(ball->GetPositionable()->GetPosition().y, -1.0f) << "the ball landed on a floor it shouldn't touch";
        EXPECT_EQ(ballEvents->collisionEnter, 0);
        EXPECT_EQ(floorEvents->collisionEnter, 0);
    }

    TYPED_TEST(LayerPhysicsTest, TurningCollisionOffMidContactEndsThePair)
    {
        const auto floor = this->SpawnFloorOn(LayerA);
        auto *floorEvents = floor->template AddComponent<LayerEventCounter>();
        const auto ball = this->SpawnBallOn("Lander", Vector3(0.0f, 2.0f, 0.0f), LayerB);
        auto *ballEvents = ball->template AddComponent<LayerEventCounter>();

        this->Step(180); // lands and comes to rest
        ASSERT_GT(ballEvents->collisionEnter, ballEvents->collisionExit) << "the ball should be resting on the floor";

        Layers::SetCollision(LayerA, LayerB, false);
        this->Step(1);
        EXPECT_EQ(ballEvents->collisionExit, ballEvents->collisionEnter) << "the suppressed pair never exited";
        EXPECT_EQ(floorEvents->collisionExit, floorEvents->collisionEnter);
        const int stays = ballEvents->collisionStay;

        this->Step(60);
        EXPECT_EQ(ballEvents->collisionStay, stays) << "the suppressed pair still stays";
        EXPECT_LT(ball->GetPositionable()->GetPosition().y, 0.0f) << "the ball should have fallen through";
    }

    TYPED_TEST(LayerPhysicsTest, TriggersObeyTheMatrix)
    {
        Layers::SetCollision(LayerA, LayerB, false);
        const auto zone = this->SpawnZone(LayerA);
        auto *zoneEvents = zone->template AddComponent<LayerEventCounter>();
        const auto visitor = this->SpawnVisitor(LayerB);
        auto *visitorEvents = visitor->template AddComponent<LayerEventCounter>();

        this->Step(5);

        EXPECT_EQ(zoneEvents->triggerEnter, 0) << "a trigger fired between layers that don't collide";
        EXPECT_EQ(visitorEvents->triggerEnter, 0);
    }

    TYPED_TEST(LayerPhysicsTest, MatrixChangesEndAndRestartOverlappingTriggers)
    {
        const auto zone = this->SpawnZone(LayerA);
        auto *zoneEvents = zone->template AddComponent<LayerEventCounter>();
        const auto visitor = this->SpawnVisitor(LayerB);
        auto *visitorEvents = visitor->template AddComponent<LayerEventCounter>();

        this->Step(5);
        ASSERT_EQ(zoneEvents->triggerEnter, 1);
        ASSERT_EQ(visitorEvents->triggerEnter, 1);

        Layers::SetCollision(LayerA, LayerB, false);
        this->Step(1);
        const int stays = zoneEvents->triggerStay;
        this->Step(5);

        EXPECT_EQ(zoneEvents->triggerExit, 1);
        EXPECT_EQ(visitorEvents->triggerExit, 1);
        EXPECT_EQ(zoneEvents->triggerStay, stays) << "the suppressed pair still stays";

        // Still overlapping, so allowing it again starts it again
        Layers::SetCollision(LayerA, LayerB, true);
        this->Step(5);

        EXPECT_EQ(zoneEvents->triggerEnter, 2);
        EXPECT_EQ(visitorEvents->triggerEnter, 2);
        EXPECT_EQ(zoneEvents->triggerExit, 1);
    }

    TYPED_TEST(LayerPhysicsTest, ChangingAnObjectsLayerAtRuntimeRefilters)
    {
        Layers::SetCollision(LayerA, LayerB, false);
        const auto zone = this->SpawnZone(LayerA);
        auto *zoneEvents = zone->template AddComponent<LayerEventCounter>();
        const auto visitor = this->SpawnVisitor(LayerC);
        auto *visitorEvents = visitor->template AddComponent<LayerEventCounter>();

        this->Step(5);
        ASSERT_EQ(visitorEvents->triggerEnter, 1);

        // To a layer the zone doesn't collide with: the pair ends
        visitor->SetLayer(LayerB);
        this->Step(5);
        EXPECT_EQ(visitorEvents->triggerExit, 1);
        EXPECT_EQ(zoneEvents->triggerExit, 1);

        // Back to one it does: it starts again
        visitor->SetLayer(LayerC);
        this->Step(5);
        EXPECT_EQ(visitorEvents->triggerEnter, 2);
        EXPECT_EQ(zoneEvents->triggerEnter, 2);

        // To another layer that still collides: the pair carries on, with no Exit and Enter in between
        const int stays = zoneEvents->triggerStay;
        visitor->SetLayer(Layers::Default);
        this->Step(5);
        EXPECT_EQ(visitorEvents->triggerExit, 1);
        EXPECT_EQ(visitorEvents->triggerEnter, 2);
        EXPECT_EQ(zoneEvents->triggerExit, 1);
        EXPECT_EQ(zoneEvents->triggerEnter, 2);
        EXPECT_GT(zoneEvents->triggerStay, stays);
    }

    // A change that keeps a touching contact pair allowed must be invisible to scripts, also when the ball
    // has gone to sleep on the floor (PhysX: resetFiltering wakes only the actor it's called on; Jolt: a sleeping body
    // has no contacts of its own to report, and a pair it was in must not end and start again when it wakes)
    template <typename Tag>
    class LayerPhysicsRestingTest : public LayerPhysicsTest<Tag>
    {
    protected:
        GameObject::Ptr _floor, _ball;
        LayerEventCounter *_floorEvents = nullptr, *_ballEvents = nullptr;

        void LandAndSleep()
        {
            _floor = this->SpawnFloorOn(LayerA);
            _floorEvents = _floor->template AddComponent<LayerEventCounter>();
            _ball = this->SpawnBallOn("Sleeper", Vector3(0.0f, 2.0f, 0.0f), LayerB);
            _ballEvents = _ball->template AddComponent<LayerEventCounter>();
            this->Step(240); // four seconds: it lands, settles and falls asleep
            ASSERT_GT(_ballEvents->collisionEnter, _ballEvents->collisionExit)
                << "the ball should be resting on the floor";
        }

        void ExpectPairUndisturbed(const int steps)
        {
            const int ballEnter = _ballEvents->collisionEnter, ballExit = _ballEvents->collisionExit;
            const int floorEnter = _floorEvents->collisionEnter, floorExit = _floorEvents->collisionExit;
            const int stays = _ballEvents->collisionStay;

            for (int i = 1; i <= steps; ++i)
            {
                this->Step(1);
                EXPECT_EQ(_ballEvents->collisionExit, ballExit) << "spurious Exit after step " << i;
                EXPECT_EQ(_ballEvents->collisionEnter, ballEnter) << "spurious Enter after step " << i;
                EXPECT_EQ(_floorEvents->collisionExit, floorExit) << "spurious Exit after step " << i;
                EXPECT_EQ(_floorEvents->collisionEnter, floorEnter) << "spurious Enter after step " << i;
                EXPECT_EQ(_ballEvents->collisionStay, stays + i) << "Stay stopped after step " << i;
            }
            EXPECT_GT(_ball->GetPositionable()->GetPosition().y, 0.5f) << "the ball fell through the floor";
        }
    };

    TYPED_TEST_SUITE(LayerPhysicsRestingTest, AllBackends, BackendNames);

    TYPED_TEST(LayerPhysicsRestingTest, FloorMovedToAnotherCollidingLayer)
    {
        this->LandAndSleep();
        this->_floor->SetLayer(LayerC);
        this->ExpectPairUndisturbed(3);
    }

    TYPED_TEST(LayerPhysicsRestingTest, BallMovedToAnotherCollidingLayer)
    {
        this->LandAndSleep();
        this->_ball->SetLayer(LayerC);
        this->ExpectPairUndisturbed(3);
    }

    TYPED_TEST(LayerPhysicsRestingTest, MatrixChangeThatKeepsThePairAllowed)
    {
        this->LandAndSleep();
        Layers::SetCollision(LayerA, LayerC, false); // rewrites the floor's row; A and B still collide
        this->ExpectPairUndisturbed(3);
    }

    TYPED_TEST(LayerPhysicsTest, LayerSetWhileTheBodyIsDisabledAppliesWhenItIsEnabled)
    {
        Layers::SetCollision(LayerA, LayerB, false);
        const auto zone = this->SpawnZone(LayerA);
        auto *zoneEvents = zone->template AddComponent<LayerEventCounter>();
        const auto visitor = this->SpawnVisitor(LayerC);
        auto *body = visitor->template GetComponent<Rigidbody>();

        this->Step(5);
        ASSERT_EQ(zoneEvents->triggerEnter, 1);

        body->SetActive(false); // the body leaves the scene; its shapes stay on it
        this->Step(2);
        ASSERT_EQ(zoneEvents->triggerExit, 1);

        visitor->SetLayer(LayerB);
        body->SetActive(true);
        this->Step(5);
        EXPECT_EQ(zoneEvents->triggerEnter, 1) << "the re-enabled body kept its old filter data";
        if constexpr (!TypeParam::kIsJolt) // a query: Jolt has none before J5
        {
            EXPECT_FALSE(Raycast::Any(Vector3(0.0f, 5.0f, 0.0f), Downward, 100.0f, Layers::MaskOf(LayerC)));
        }

        // And the same through the object: its collider's shapes are re-created on the new layer
        visitor->SetActive(false);
        this->Step(1);
        visitor->SetLayer(LayerC);
        visitor->SetActive(true);
        this->Step(5);
        EXPECT_EQ(zoneEvents->triggerEnter, 2);
    }

    TYPED_TEST(LayerPhysicsTest, CollidersAttachedAfterAMatrixChangeUseTheNewMatrix)
    {
        const auto floor = this->SpawnFloorOn(LayerA);
        this->Step(1);

        Layers::SetCollision(LayerA, LayerB, false);
        const auto ball = this->SpawnBallOn("Late", Vector3(0.0f, 2.0f, 0.0f), LayerB);
        auto *ballEvents = ball->template AddComponent<LayerEventCounter>();
        this->Step(90);

        EXPECT_EQ(ballEvents->collisionEnter, 0);
        EXPECT_LT(ball->GetPositionable()->GetPosition().y, -1.0f);
    }

#undef N2_SKIP_QUERIES_ON_JOLT
}

#endif // a physics backend is in this build
