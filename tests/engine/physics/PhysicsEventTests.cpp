// Contact and trigger events, as TYPED tests over every backend this build has (PhysX and Jolt): the data an Enter
// carries, a body that mixes solid and trigger colliders, a compound body's one Enter, and the order of the events (the
// same scene run twice gives the same sequence). Both backends deliver events through the ContactTracker, so what
// differs between them is only what each physics engine reports (docs/physics.html, "Jolt backend", differences). The
// layer matrix and its runtime changes are in LayerPhysicsTests.cpp, the iteration order of the touch maps in
// PhysicsOrderTests.cpp.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <format>
#include <string>
#include <vector>

#include "engine/Component.hpp"
#include "engine/Layers.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        // Counts the events its object gets and keeps the first collision Enter's data
        class ContactRecorder final : public Component
        {
        public:
            explicit ContactRecorder(GameObject &gameObject) : Component(gameObject) {}
            [[nodiscard]] std::string GetTypeName() const override { return "ContactRecorder"; }

            void OnCollisionEnter(const Collision &c) override
            {
                if (collisionEnter == 0)
                {
                    contactCount = c.contacts.size();
                    if (!c.contacts.empty())
                    {
                        first = c.contacts.front();
                    }
                    impulse = c.impulse;
                    relativeVelocity = c.relativeVelocity;
                    other = c.otherGameObject;
                    collider = c.collider;
                    otherCollider = c.otherCollider;
                }
                ++collisionEnter;
            }
            void OnCollisionStay(const Collision &) override { ++collisionStay; }
            void OnCollisionExit(const Collision &) override { ++collisionExit; }
            void OnTriggerEnter(const Trigger) override { ++triggerEnter; }
            void OnTriggerStay(const Trigger) override { ++triggerStay; }
            void OnTriggerExit(const Trigger) override { ++triggerExit; }

            int collisionEnter = 0, collisionStay = 0, collisionExit = 0;
            int triggerEnter = 0, triggerStay = 0, triggerExit = 0;
            std::size_t contactCount = 0;
            ContactPoint first;
            Vector3 impulse = Vector3::Zero, relativeVelocity = Vector3::Zero;
            GameObject *other = nullptr;
            ICollider *collider = nullptr, *otherCollider = nullptr;
        };

        // Writes every event it gets, with the step it came in, to the log of the run in progress
        std::vector<std::string> *g_log = nullptr;
        int g_step = 0;

        class SequenceRecorder final : public Component
        {
        public:
            explicit SequenceRecorder(GameObject &gameObject) : Component(gameObject) {}
            [[nodiscard]] std::string GetTypeName() const override { return "SequenceRecorder"; }

            void OnCollisionEnter(const Collision &c) override { Log("CollisionEnter", c.otherGameObject); }
            void OnCollisionStay(const Collision &c) override { Log("CollisionStay", c.otherGameObject); }
            void OnCollisionExit(const Collision &c) override { Log("CollisionExit", c.otherGameObject); }
            void OnTriggerEnter(const Trigger t) override { Log("TriggerEnter", t.otherGameObject); }
            void OnTriggerStay(const Trigger t) override { Log("TriggerStay", t.otherGameObject); }
            void OnTriggerExit(const Trigger t) override { Log("TriggerExit", t.otherGameObject); }

        private:
            void Log(const char *kind, const GameObject *other) const
            {
                if (g_log)
                {
                    g_log->push_back(std::format("{} {} {} {}", g_step, GetGameObject().GetName(), kind,
                                                 other ? other->GetName() : std::string("-")));
                }
            }
        };
    }

    template <typename Tag>
    class PhysicsEventTest : public PhysicsCoreFixture<Tag>
    {
    };

    TYPED_TEST_SUITE(PhysicsEventTest, AllBackends, BackendNames);

    // ============================================================================
    // What an Enter carries
    // ============================================================================

    TYPED_TEST(PhysicsEventTest, CollisionEnterCarriesTheContactFromEachSidesPointOfView)
    {
        const auto floor = this->SpawnFloor();
        auto *floorEvents = floor->template AddComponent<ContactRecorder>();
        const auto ball = this->SpawnBall("Ball", Vector3(0.0f, 3.0f, 0.0f));
        auto *ballEvents = ball->template AddComponent<ContactRecorder>();

        this->Step(120);

        ASSERT_GE(ballEvents->collisionEnter, 1);
        ASSERT_GE(floorEvents->collisionEnter, 1);
        ASSERT_GE(ballEvents->contactCount, 1u);
        ASSERT_GE(floorEvents->contactCount, 1u);
        EXPECT_EQ(ballEvents->other, floor.get());
        EXPECT_EQ(floorEvents->other, ball.get());
        EXPECT_EQ(ballEvents->otherCollider, floorEvents->collider);
        EXPECT_EQ(ballEvents->collider, floorEvents->otherCollider);

        // The normal points from the other body to the receiver: up for the ball, down for the floor
        EXPECT_GT(ballEvents->first.normal.y, 0.9f);
        EXPECT_LT(floorEvents->first.normal.y, -0.9f);
        // The contact is on the floor's top surface (y = 0.5), touching or within the contact offset
        EXPECT_NEAR(ballEvents->first.point.y, 0.5f, 0.1f);
        EXPECT_LT(ballEvents->first.separation, 0.05f);
        // An impulse that pushes the ball up (Jolt: an estimate from the velocities before the solve, PhysX: the applied
        // one), and the same for the floor from its own side
        EXPECT_GT(ballEvents->first.normalImpulse, 0.0f);
        EXPECT_GT(ballEvents->impulse.y, 0.0f);
        EXPECT_LT(floorEvents->impulse.y, 0.0f);
        // relativeVelocity is not compared in its direction: PhysX samples it after the step (the ball may be bouncing
        // by then), Jolt before the solve (the impact velocity). Each side sees the opposite of the other's.
        EXPECT_NEAR(ballEvents->relativeVelocity.x, -floorEvents->relativeVelocity.x, 1e-4f);
        EXPECT_NEAR(ballEvents->relativeVelocity.y, -floorEvents->relativeVelocity.y, 1e-4f);
    }

    TYPED_TEST(PhysicsEventTest, ACompoundBodyEntersOnceThroughItsTwoColliders)
    {
        this->SpawnFloor();
        const auto go = this->Spawn("Compound", Vector3(0.0f, 1.02f, 0.0f)); // 2 cm above the floor
        auto *left = go->template AddComponent<BoxCollider>();
        left->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        left->SetOffset(Vector3(-0.6f, 0.0f, 0.0f));
        auto *right = go->template AddComponent<BoxCollider>();
        right->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        right->SetOffset(Vector3(0.6f, 0.0f, 0.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        body->SetGravityEnabled(true);
        auto *events = go->template AddComponent<ContactRecorder>();

        this->Step(60);

        EXPECT_EQ(events->collisionEnter, 1) << "one Enter per body pair, not one per collider pair";
        EXPECT_EQ(events->collisionExit, 0);
        EXPECT_GE(events->collisionStay, 50);
    }

    // ============================================================================
    // A body that mixes solid and trigger colliders
    // ============================================================================

    TYPED_TEST(PhysicsEventTest, ATriggerColliderOnABodyDoesNotHoldItsSolidCollidersUp)
    {
        this->SpawnFloor();
        const auto go = this->Spawn("Mixed", Vector3(0.0f, 3.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *aura = go->template AddComponent<SphereCollider>();
        aura->SetRadius(1.5f);
        aura->SetIsTrigger(true);
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        body->SetGravityEnabled(true);
        auto *events = go->template AddComponent<ContactRecorder>();

        this->Step(150);

        // The box rests on its own bottom (y = 1.0); a sphere of radius 1.5 that collided would hold it at y = 2.0
        EXPECT_NEAR(go->GetPositionable()->GetPosition().y, 1.0f, 0.06f);
        EXPECT_GE(events->collisionEnter, 1) << "the solid box still collides with the floor";
    }

    TYPED_TEST(PhysicsEventTest, ATriggerColliderOnABodySeesAVisitorThatTheSolidOnesDoNot)
    {
        // A weightless body with a solid box and a trigger sphere (radius 2) around it, and a weightless ball in the
        // sphere but outside the box: trigger events for both, no collision events, and neither moves the other
        const auto go = this->Spawn("Mixed", Vector3(0.0f, 10.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *aura = go->template AddComponent<SphereCollider>();
        aura->SetRadius(2.0f);
        aura->SetIsTrigger(true);
        this->AddFloatingBody(*go, 3.0f);
        auto *mixedEvents = go->template AddComponent<ContactRecorder>();

        const auto visitor = this->Spawn("Visitor", Vector3(1.5f, 10.0f, 0.0f));
        visitor->template AddComponent<SphereCollider>()->SetRadius(0.25f);
        auto *visitorBody = this->AddFloatingBody(*visitor, 1.0f);
        auto *visitorEvents = visitor->template AddComponent<ContactRecorder>();

        this->Step(6);
        EXPECT_EQ(mixedEvents->triggerEnter, 1);
        EXPECT_EQ(visitorEvents->triggerEnter, 1);
        EXPECT_GE(mixedEvents->triggerStay, 1);
        EXPECT_EQ(mixedEvents->collisionEnter, 0) << "the visitor is outside the box";
        EXPECT_EQ(visitorEvents->collisionEnter, 0);

        // It leaves along +x, past the sphere (distance 1.5 + 0.25 from the centre to 2.25 and beyond)
        visitorBody->SetVelocity(Vector3(5.0f, 0.0f, 0.0f));
        this->Step(60);
        EXPECT_EQ(mixedEvents->triggerExit, 1);
        EXPECT_EQ(visitorEvents->triggerExit, 1);
        EXPECT_EQ(mixedEvents->collisionEnter, 0);
        EXPECT_NEAR(go->GetPositionable()->GetPosition().x, 0.0f, 1e-3f) << "a trigger collider pushed the body";
        EXPECT_NEAR(go->GetPositionable()->GetPosition().y, 10.0f, 1e-3f);
    }

    // ============================================================================
    // Which pairs raise events, and when a pair ends
    // ============================================================================

    TYPED_TEST(PhysicsEventTest, AKinematicBodyTriggersAStaticZoneAndMeetsASolidBoxSilently)
    {
        // Both backends: a kinematic body entering and leaving a static trigger zone raises trigger events, and meets a
        // static solid box (also in its path) with no collision events.
        const auto zone = this->Spawn("Zone", Vector3(0.0f, 0.0f, 0.0f));
        auto *zoneCollider = zone->template AddComponent<BoxCollider>();
        zoneCollider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
        zoneCollider->SetIsTrigger(true);
        auto *zoneEvents = zone->template AddComponent<ContactRecorder>();
        const auto wall = this->Spawn("Wall", Vector3(0.0f, 0.0f, 4.5f));
        wall->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *wallEvents = wall->template AddComponent<ContactRecorder>();
        const auto mover = this->Spawn("Mover", Vector3(0.0f, 0.0f, -5.0f));
        mover->template AddComponent<SphereCollider>()->SetRadius(0.5f);
        mover->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
        auto *moverEvents = mover->template AddComponent<ContactRecorder>();

        for (int i = 0; i < 60; ++i) // from z = -5 to z = 6.8: through the zone
        {
            mover->GetPositionable()->SetPosition(Vector3(0.0f, 0.0f, -5.0f + 0.2f * static_cast<float>(i)));
            this->Step(1);
        }

        EXPECT_EQ(zoneEvents->triggerEnter, 1);
        EXPECT_EQ(moverEvents->triggerEnter, 1);
        EXPECT_GE(zoneEvents->triggerStay, 5);
        EXPECT_EQ(zoneEvents->triggerExit, 1);
        EXPECT_EQ(moverEvents->triggerExit, 1);
        EXPECT_EQ(wallEvents->collisionEnter, 0);
        EXPECT_EQ(moverEvents->collisionEnter, 0);
    }

    TYPED_TEST(PhysicsEventTest, DisablingABodyEndsItsPairsWithAnExit)
    {
        const auto floor = this->SpawnFloor();
        auto *floorEvents = floor->template AddComponent<ContactRecorder>();
        const auto ball = this->SpawnBall("Ball", Vector3(0.0f, 1.0f, 0.0f)); // rests on the floor
        this->Step(60);
        ASSERT_GE(floorEvents->collisionEnter, 1);
        ASSERT_GT(floorEvents->collisionEnter, floorEvents->collisionExit) << "the ball should be touching the floor";

        ball->template GetComponent<Rigidbody>()->SetActive(false); // the body leaves the simulation
        this->Step(2);
        EXPECT_EQ(floorEvents->collisionExit, floorEvents->collisionEnter) << "the pair never ended";
        const int stays = floorEvents->collisionStay;
        this->Step(5);
        EXPECT_EQ(floorEvents->collisionStay, stays) << "the ended pair still stays";
    }

    // ============================================================================
    // The order of events
    // ============================================================================

    // A fresh backend per run: handle indices come from a free list, so a shared backend would give the second run
    // recycled indices, and the order of events follows the handles
    template <typename Tag>
    class PhysicsEventSequenceTest : public ::testing::Test
    {
    protected:
        using Backend = typename Tag::Backend;

        void TearDown() override
        {
            g_log = nullptr;
            SceneManager::AddScene(Scene::Create("PhysicsEventSequence_Teardown"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
        }

        // Falling balls and a box through a trigger zone onto a floor and onto each other: collisions, triggers, Stay,
        // Exit and bounces. The events (with the step each came in) are returned in the order they were delivered.
        std::vector<std::string> RunScene(const bool deterministic)
        {
            // The previous run's backend goes first: PhysX allows one foundation per process
            SceneManager::AddScene(Scene::Create("PhysicsEventSequence_Between"), true);
            SceneManager::ProcessAnyPendingSceneChange();
            Application::GetInstance().Set3DPhysicsBackend(nullptr);
            auto backend = std::make_unique<Backend>();
            backend->SetDeterministic(deterministic);
            EXPECT_TRUE(backend->Initialize());
            Application::GetInstance().Set3DPhysicsBackend(std::move(backend));
            Layers::ResetToDefaults();
            SceneManager::AddScene(Scene::Create(std::string("PhysicsEventSequence_") +
                                                 ::testing::UnitTest::GetInstance()->current_test_info()->name()),
                                   true);
            SceneManager::ProcessAnyPendingSceneChange();
            Scene *scene = SceneManager::GetCurScene();

            std::vector<std::string> log;
            g_log = &log;
            g_step = 0;

            const auto spawn = [scene](const std::string &name, const Vector3 &position)
            {
                const auto go = GameObject::Create(name);
                go->CreatePositionable();
                go->GetPositionable()->SetPosition(position);
                scene->AddRootGameObject(go);
                go->template AddComponent<SequenceRecorder>();
                return go;
            };

            const auto floor = spawn("Floor", Vector3(0.0f, 0.0f, 0.0f));
            floor->template AddComponent<BoxCollider>()->SetSize(Vector3(20.0f, 1.0f, 20.0f));

            const auto zone = spawn("Zone", Vector3(0.0f, 2.0f, 0.0f));
            auto *zoneCollider = zone->template AddComponent<BoxCollider>();
            zoneCollider->SetSize(Vector3(6.0f, 2.0f, 6.0f));
            zoneCollider->SetIsTrigger(true);

            for (int i = 0; i < 10; ++i)
            {
                const float x = (static_cast<float>((i * 7) % 10) - 5.0f) * 0.6f;
                const float z = (static_cast<float>((i * 3) % 10) - 5.0f) * 0.6f;
                const auto ball = spawn("Ball" + std::to_string(i), Vector3(x, 3.0f + 0.9f * static_cast<float>(i), z));
                ball->template AddComponent<SphereCollider>()->SetRadius(0.5f);
                auto *body = ball->template AddComponent<Rigidbody>();
                body->SetBodyType(BodyType::Dynamic);
                body->SetGravityEnabled(true);
            }
            const auto crate = spawn("Crate", Vector3(0.3f, 14.0f, 0.2f));
            crate->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
            auto *crateBody = crate->template AddComponent<Rigidbody>();
            crateBody->SetBodyType(BodyType::Dynamic);
            crateBody->SetGravityEnabled(true);

            IPhysicsBackend &physics = *Application::GetInstance().Get3DPhysicsBackend();
            for (g_step = 1; g_step <= 300; ++g_step)
            {
                scene->ProcessAttachQueue();
                physics.ApplyPendingChanges();
                scene->FixedUpdate();
                physics.Update(1.0f / 60.0f);
                physics.SyncTransforms();
                physics.ProcessCollisionCallbacks();
                scene->ProcessDestroyed();
            }
            g_log = nullptr;
            return log;
        }

        static std::size_t Count(const std::vector<std::string> &log, const std::string &kind)
        {
            std::size_t count = 0;
            for (const std::string &line : log)
            {
                count += line.find(" " + kind + " ") != std::string::npos ? 1u : 0u;
            }
            return count;
        }
    };

    TYPED_TEST_SUITE(PhysicsEventSequenceTest, AllBackends, BackendNames);

    TYPED_TEST(PhysicsEventSequenceTest, TheSameSceneRunTwiceDeliversTheSameEventSequence)
    {
        const std::vector<std::string> first = this->RunScene(true);
        const std::vector<std::string> second = this->RunScene(true);

        // The scene did something: bodies met the floor and each other, passed through the zone, and left it
        EXPECT_GT(this->Count(first, "CollisionEnter"), 10u);
        EXPECT_GT(this->Count(first, "CollisionStay"), 10u);
        EXPECT_GT(this->Count(first, "TriggerEnter"), 5u);
        EXPECT_GT(this->Count(first, "TriggerExit"), 5u);
        EXPECT_EQ(first, second);
    }

    // Jolt's contact listener runs on worker threads and reports in no fixed order; the backend sorts what it
    // reports. With the thread pool (physics.deterministic off) the sequence must still repeat.
    TYPED_TEST(PhysicsEventSequenceTest, TheSameSceneOnTheThreadPoolDeliversTheSameEventSequence)
    {
        if constexpr (!TypeParam::kIsJolt)
        {
            GTEST_SKIP() << "PhysX's own report order is only fixed with physics.deterministic (the test above)";
        }
        const std::vector<std::string> first = this->RunScene(false);
        const std::vector<std::string> second = this->RunScene(false);
        EXPECT_GT(this->Count(first, "CollisionEnter"), 10u);
        EXPECT_EQ(first, second);
    }
}

#endif // a physics backend is in this build
