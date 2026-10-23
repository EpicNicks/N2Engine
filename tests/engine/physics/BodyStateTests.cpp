// IPhysicsBackend::GetBodyState and SetBodyState (netcode B5), as TYPED tests over every backend this build has
// (PhysX and Jolt): what each kind of body takes, the exact round trip a client reconciliation relies on, what is
// refused, and what a teleport does to the events (a body teleported into a trigger raises one Enter, one teleported
// out raises one Exit, and writing back the state a body already has raises nothing).
//
// The raw-backend suite (BodyStateBackendTest) needs no scene. A difference between the backends that a test has to
// allow for is written next to the assertion and listed in docs/physics.html (the Jolt backend, differences).

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <cmath>
#include <limits>
#include <string>

#include "engine/Component.hpp"
#include "engine/physics/BodyState.hpp"

namespace N2PhysicsTests
{
    namespace
    {
        constexpr float kDt = 1.0f / 60.0f;

        // A rotation of `degrees` about the y axis, as (w, x, y, z)
        Math::Quaternion AboutY(const float degrees)
        {
            const float half = degrees * 3.14159265358979f / 360.0f;
            return Math::Quaternion(std::cos(half), 0.0f, std::sin(half), 0.0f);
        }

        // Equal to float rounding (a backend keeps a dynamic body's pose relative to its centre of mass, so a pose read
        // back may differ from the one written in the last bit)
        void ExpectSameState(const BodyState &a, const BodyState &b, const float tolerance = 1e-6f)
        {
            ExpectVectorNear(a.position, b.position, tolerance);
            EXPECT_NEAR(a.rotation.GetW(), b.rotation.GetW(), tolerance);
            EXPECT_NEAR(a.rotation.GetX(), b.rotation.GetX(), tolerance);
            EXPECT_NEAR(a.rotation.GetY(), b.rotation.GetY(), tolerance);
            EXPECT_NEAR(a.rotation.GetZ(), b.rotation.GetZ(), tolerance);
            ExpectVectorNear(a.linearVelocity, b.linearVelocity, tolerance);
            ExpectVectorNear(a.angularVelocity, b.angularVelocity, tolerance);
            EXPECT_EQ(a.sleeping, b.sleeping);
        }

        // Counts the trigger and collision events its object gets
        class StateEventRecorder final : public Component
        {
        public:
            explicit StateEventRecorder(GameObject &gameObject) : Component(gameObject) {}
            [[nodiscard]] std::string GetTypeName() const override { return "StateEventRecorder"; }

            void OnCollisionEnter(const Collision &) override { ++collisionEnter; }
            void OnCollisionExit(const Collision &) override { ++collisionExit; }
            void OnTriggerEnter(const Trigger) override { ++triggerEnter; }
            void OnTriggerExit(const Trigger) override { ++triggerExit; }

            int collisionEnter = 0, collisionExit = 0;
            int triggerEnter = 0, triggerExit = 0;
        };
    }

    // ============================================================================
    // A bare backend: no scene, bodies made through IPhysicsBackend alone
    // ============================================================================

    template <typename Tag>
    class BodyStateBackendTest : public ::testing::Test
    {
    protected:
        using Backend = typename Tag::Backend;

        void SetUp() override { ASSERT_TRUE(backend.Initialize()); }

        PhysicsBodyHandle MakeBall(const Vector3 &position, const bool kinematic = false)
        {
            const PhysicsBodyHandle body =
                backend.CreateDynamicBody(position, Math::Quaternion::Identity, 1.0f, nullptr, kinematic);
            backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial::Default());
            return body;
        }

        Backend backend;
    };

    TYPED_TEST_SUITE(BodyStateBackendTest, AllBackends, BackendNames);

    TYPED_TEST(BodyStateBackendTest, SaysItCanSetStateAndNamesItself)
    {
        EXPECT_TRUE(this->backend.SupportsSetState());
        EXPECT_EQ(this->backend.BackendId(), TypeParam::kIsJolt ? "jolt" : "physx");
    }

    TYPED_TEST(BodyStateBackendTest, ADynamicBodyIsTeleportedWithItsVelocitiesAndReadsBack)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(0.0f, 10.0f, 0.0f));

        BodyState wanted;
        wanted.position = Vector3(3.0f, 4.5f, -2.25f);
        wanted.rotation = AboutY(90.0f);
        wanted.linearVelocity = Vector3(1.0f, -2.0f, 0.5f);
        wanted.angularVelocity = Vector3(0.25f, 0.5f, -0.75f);
        wanted.sleeping = false;
        ASSERT_TRUE(this->backend.SetBodyState(body, wanted));

        BodyState got;
        ASSERT_TRUE(this->backend.GetBodyState(body, got));
        ExpectSameState(got, wanted);
        // The same numbers through the older accessors
        EXPECT_NEAR(this->backend.GetPosition(body).y, 4.5f, 1e-6f);
        EXPECT_NEAR(this->backend.GetVelocity(body).x, 1.0f, 1e-6f);
        EXPECT_NEAR(this->backend.GetAngularVelocity(body).z, -0.75f, 1e-6f);
    }

    TYPED_TEST(BodyStateBackendTest, ReadingAStateAndWritingItBackChangesNothing)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(1.0f, 20.0f, 2.0f));
        BodyState wanted;
        wanted.position = Vector3(1.5f, 20.25f, -4.0f);
        wanted.rotation = AboutY(30.0f);
        wanted.linearVelocity = Vector3(0.5f, 1.0f, -1.5f);
        wanted.angularVelocity = Vector3(0.0f, 2.0f, 0.0f);
        ASSERT_TRUE(this->backend.SetBodyState(body, wanted));

        // Again and again: what is read is what is written, with no drift from one round to the next
        BodyState first;
        ASSERT_TRUE(this->backend.GetBodyState(body, first));
        BodyState current = first;
        for (int round = 0; round < 8; ++round)
        {
            ASSERT_TRUE(this->backend.SetBodyState(body, current));
            ASSERT_TRUE(this->backend.GetBodyState(body, current));
        }
        ExpectSameState(current, first, 1e-6f);
    }

    TYPED_TEST(BodyStateBackendTest, ATeleportedBodyKeepsSimulatingFromTheNewState)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(0.0f, 10.0f, 0.0f));
        BodyState wanted;
        wanted.position = Vector3(5.0f, 20.0f, 0.0f);
        wanted.linearVelocity = Vector3(6.0f, 0.0f, 0.0f);
        ASSERT_TRUE(this->backend.SetBodyState(body, wanted));

        this->backend.Update(kDt);
        BodyState after;
        ASSERT_TRUE(this->backend.GetBodyState(body, after));
        EXPECT_FALSE(after.sleeping);
        EXPECT_NEAR(after.position.x, 5.0f + 6.0f * kDt, 1e-3f) << "moved on from the teleport target";
        EXPECT_LT(after.position.y, 20.0f) << "and gravity pulls it down from there";
        EXPECT_GT(after.position.y, 19.9f);
        EXPECT_NEAR(after.linearVelocity.x, 6.0f, 5e-2f) << "less a little damping";
    }

    TYPED_TEST(BodyStateBackendTest, ARotationThatIsNotUnitLengthIsNormalised)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(0.0f, 1.0f, 0.0f));
        BodyState wanted;
        wanted.position = Vector3(0.0f, 1.0f, 0.0f);
        wanted.rotation = Math::Quaternion(2.0f, 0.0f, 0.0f, 0.0f); // w = 2: twice the identity
        ASSERT_TRUE(this->backend.SetBodyState(body, wanted));

        BodyState got;
        ASSERT_TRUE(this->backend.GetBodyState(body, got));
        EXPECT_NEAR(got.rotation.GetW(), 1.0f, 1e-6f);
        EXPECT_NEAR(got.rotation.GetX(), 0.0f, 1e-6f);
    }

    TYPED_TEST(BodyStateBackendTest, ASleepRequestPutsTheBodyToSleepWithNoVelocityAndAWakeRequestWakesIt)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(0.0f, 10.0f, 0.0f));
        BodyState asleep;
        asleep.position = Vector3(1.0f, 10.0f, 2.0f);
        asleep.linearVelocity = Vector3(3.0f, 0.0f, 0.0f); // a sleeping body keeps none
        asleep.sleeping = true;
        ASSERT_TRUE(this->backend.SetBodyState(body, asleep));

        // PhysX applies the island change when the scene steps, Jolt at once; the body must not move in the step
        this->backend.Update(kDt);
        BodyState got;
        ASSERT_TRUE(this->backend.GetBodyState(body, got));
        EXPECT_TRUE(got.sleeping);
        ExpectVectorNear(got.position, Vector3(1.0f, 10.0f, 2.0f), 1e-5f);
        ExpectVectorNear(got.linearVelocity, Vector3::Zero, 0.0f);

        BodyState awake = got;
        awake.sleeping = false;
        awake.linearVelocity = Vector3(0.0f, 0.0f, 4.0f);
        ASSERT_TRUE(this->backend.SetBodyState(body, awake));
        this->backend.Update(kDt);
        ASSERT_TRUE(this->backend.GetBodyState(body, got));
        EXPECT_FALSE(got.sleeping);
        EXPECT_GT(got.position.z, 2.0f) << "it moves again";
    }

    TYPED_TEST(BodyStateBackendTest, AKinematicBodyTakesOnlyThePoseAsATargetAndReportsNoVelocity)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(0.0f, 5.0f, 0.0f), true);
        BodyState wanted;
        wanted.position = Vector3(2.0f, 6.0f, -1.0f);
        wanted.rotation = AboutY(45.0f);
        wanted.linearVelocity = Vector3(100.0f, 0.0f, 0.0f); // does not apply to a kinematic body
        wanted.sleeping = true;                              // nor does this
        ASSERT_TRUE(this->backend.SetBodyState(body, wanted));

        this->backend.Update(kDt);
        BodyState got;
        ASSERT_TRUE(this->backend.GetBodyState(body, got));
        ExpectVectorNear(got.position, Vector3(2.0f, 6.0f, -1.0f), 1e-4f);
        EXPECT_NEAR(got.rotation.GetY(), wanted.rotation.GetY(), 1e-4f);
        ExpectVectorNear(got.linearVelocity, Vector3::Zero, 0.0f);
        ExpectVectorNear(got.angularVelocity, Vector3::Zero, 0.0f);
    }

    TYPED_TEST(BodyStateBackendTest, AStaticBodyTakesOnlyThePoseAndIsNeverAsleep)
    {
        const PhysicsBodyHandle ground = this->backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f),
                                                                        Math::Quaternion::Identity, nullptr);
        this->backend.AddBoxCollider(ground, nullptr, Vector3(5.0f, 0.5f, 5.0f), Vector3::Zero,
                                     PhysicsMaterial::Default());

        BodyState wanted;
        wanted.position = Vector3(10.0f, -2.0f, 3.0f);
        wanted.linearVelocity = Vector3(9.0f, 9.0f, 9.0f);
        wanted.sleeping = true;
        ASSERT_TRUE(this->backend.SetBodyState(ground, wanted));

        BodyState got;
        ASSERT_TRUE(this->backend.GetBodyState(ground, got));
        ExpectVectorNear(got.position, Vector3(10.0f, -2.0f, 3.0f), 1e-6f);
        ExpectVectorNear(got.linearVelocity, Vector3::Zero, 0.0f);
        ExpectVectorNear(got.angularVelocity, Vector3::Zero, 0.0f);
        EXPECT_FALSE(got.sleeping);
    }

    TYPED_TEST(BodyStateBackendTest, ABadStateIsRefusedAndChangesNothing)
    {
        const PhysicsBodyHandle body = this->MakeBall(Vector3(1.0f, 2.0f, 3.0f));
        BodyState before;
        ASSERT_TRUE(this->backend.GetBodyState(body, before));

        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        BodyState good;
        good.position = Vector3(9.0f, 9.0f, 9.0f);

        BodyState badPosition = good;
        badPosition.position.y = nan;
        BodyState badLinear = good;
        badLinear.linearVelocity.x = inf;
        BodyState badAngular = good;
        badAngular.angularVelocity.z = -inf;
        BodyState noRotation = good;
        noRotation.rotation = Math::Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
        BodyState nanRotation = good;
        nanRotation.rotation = Math::Quaternion(nan, 0.0f, 0.0f, 0.0f);
        for (const BodyState *bad : {&badPosition, &badLinear, &badAngular, &noRotation, &nanRotation})
        {
            EXPECT_FALSE(this->backend.SetBodyState(body, *bad));
            BodyState now;
            ASSERT_TRUE(this->backend.GetBodyState(body, now));
            ExpectSameState(now, before);
        }
    }

    TYPED_TEST(BodyStateBackendTest, AStaleInvalidOrDisabledBodyHasNoState)
    {
        BodyState out;
        out.position = Vector3(7.0f, 7.0f, 7.0f);
        EXPECT_FALSE(this->backend.GetBodyState(INVALID_PHYSICS_HANDLE, out));
        EXPECT_FALSE(this->backend.SetBodyState(INVALID_PHYSICS_HANDLE, out));
        EXPECT_EQ(out.position.x, 7.0f) << "a refused read leaves the output alone";

        const PhysicsBodyHandle body = this->MakeBall(Vector3(0.0f, 3.0f, 0.0f));
        ASSERT_TRUE(this->backend.GetBodyState(body, out));

        // Out of the scene (a deactivated object): nothing to read or write
        this->backend.SetBodyEnabled(body, false);
        out.position = Vector3(7.0f, 7.0f, 7.0f);
        EXPECT_FALSE(this->backend.GetBodyState(body, out));
        EXPECT_EQ(out.position.x, 7.0f);
        EXPECT_FALSE(this->backend.SetBodyState(body, out));
        this->backend.SetBodyEnabled(body, true);
        EXPECT_TRUE(this->backend.GetBodyState(body, out));

        // Destroyed: the handle is stale
        this->backend.DestroyBody(body);
        EXPECT_FALSE(this->backend.GetBodyState(body, out));
        EXPECT_FALSE(this->backend.SetBodyState(body, out));
    }

    // ============================================================================
    // In a scene: what a teleport does to the events
    // ============================================================================

    template <typename Tag>
    class BodyStateEventTest : public PhysicsCoreFixture<Tag>
    {
    protected:
        // A trigger box of 4 x 4 x 4 at (10, 0, 0)
        void SpawnZone()
        {
            const auto zone = this->Spawn("Zone", Vector3(10.0f, 0.0f, 0.0f));
            auto *collider = zone->template AddComponent<BoxCollider>();
            collider->SetSize(Vector3(4.0f, 4.0f, 4.0f));
            collider->SetIsTrigger(true);
        }

        // A weightless ball of radius 0.5 at `position`, created by the next Step
        GameObject::Ptr SpawnWeightlessBall(const Vector3 &position)
        {
            const auto ball = this->Spawn("Ball", position);
            ball->template AddComponent<SphereCollider>()->SetRadius(0.5f);
            this->AddFloatingBody(*ball, 1.0f);
            return ball;
        }

        bool Teleport(const GameObject &ball, const Vector3 &position)
        {
            const PhysicsBodyHandle handle = ball.template GetComponent<Rigidbody>()->GetHandle();
            BodyState state;
            if (!this->BackendRef().GetBodyState(handle, state))
            {
                return false;
            }
            state.position = position;
            return this->BackendRef().SetBodyState(handle, state);
        }
    };

    TYPED_TEST_SUITE(BodyStateEventTest, AllBackends, BackendNames);

    TYPED_TEST(BodyStateEventTest, TeleportingIntoATriggerRaisesOneEnterAndOutOfItOneExit)
    {
        this->SpawnZone();
        const auto ball = this->SpawnWeightlessBall(Vector3(0.0f, 0.0f, 0.0f));
        auto *events = ball->template AddComponent<StateEventRecorder>();
        this->Step(3);
        ASSERT_EQ(events->triggerEnter, 0);

        ASSERT_TRUE(this->Teleport(*ball, Vector3(10.0f, 0.0f, 0.0f)));
        this->Step(4);
        EXPECT_EQ(events->triggerEnter, 1) << "one Enter for the new overlap";
        EXPECT_EQ(events->triggerExit, 0);
        ExpectVectorNear(ball->GetPositionable()->GetPosition(), Vector3(10.0f, 0.0f, 0.0f), 1e-3f);

        ASSERT_TRUE(this->Teleport(*ball, Vector3(0.0f, 0.0f, 0.0f)));
        this->Step(4);
        EXPECT_EQ(events->triggerEnter, 1);
        EXPECT_EQ(events->triggerExit, 1) << "one Exit for the overlap that ended";
        EXPECT_EQ(events->collisionEnter, 0) << "a teleport through space meets no solid";
    }

    TYPED_TEST(BodyStateEventTest, WritingBackTheStateABodyAlreadyHasRaisesNoEvents)
    {
        this->SpawnZone();
        const auto ball = this->SpawnWeightlessBall(Vector3(10.0f, 0.0f, 0.0f));
        auto *events = ball->template AddComponent<StateEventRecorder>();
        this->Step(4);
        ASSERT_EQ(events->triggerEnter, 1);

        const PhysicsBodyHandle handle = ball->template GetComponent<Rigidbody>()->GetHandle();
        for (int i = 0; i < 5; ++i)
        {
            BodyState state;
            ASSERT_TRUE(this->BackendRef().GetBodyState(handle, state));
            ASSERT_TRUE(this->BackendRef().SetBodyState(handle, state));
            this->Step();
        }
        EXPECT_EQ(events->triggerEnter, 1) << "no second Enter";
        EXPECT_EQ(events->triggerExit, 0) << "and no Exit";
        EXPECT_EQ(events->collisionEnter, 0);
        EXPECT_EQ(events->collisionExit, 0);
    }
}

#endif // a physics backend is in this build
