// The core physics conformance tests, run as TYPED tests over every backend this build has (PhysX and Jolt): bodies,
// colliders, mass properties, gravity, forces and velocities, kinematic and static moves, transform sync, disabled
// bodies. The scene queries are typed over both backends too (PhysicsQueryTests.cpp, J5). The suites for layers,
// triggers and events stay PhysX-only until the Jolt backend has them (stage J4): they are PhysicsTests.cpp,
// LayerPhysicsTests.cpp and PhysicsOrderTests.cpp, each marked as such.
//
// A difference between the backends that a test has to allow for is written next to the assertion and listed in
// docs/physics.html ("Jolt backend", differences).

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

#include <algorithm>
#include <array>

#include <nlohmann/json.hpp>

namespace N2PhysicsTests
{
    template <typename Tag>
    class PhysicsCoreTest : public PhysicsCoreFixture<Tag>
    {
    };

    TYPED_TEST_SUITE(PhysicsCoreTest, AllBackends, BackendNames);

    namespace
    {
        // Solid box of full size s and mass m: I = m/12 * (sy^2 + sz^2, sx^2 + sz^2, sx^2 + sy^2)
        Vector3 BoxInertia(const float mass, const Vector3 &size)
        {
            return Vector3(size.y * size.y + size.z * size.z, size.x * size.x + size.z * size.z,
                           size.x * size.x + size.y * size.y) *
                   (mass / 12.0f);
        }

        // x of the centre of mass of a unit cube at the origin plus a radius-0.5 sphere at x = 2, same density
        constexpr float CubeAndSphereCenterX = 0.6873f; // 2 * (pi / 6) / (1 + pi / 6)
    }

    // ============================================================================
    // Motion: gravity, forces, velocities
    // ============================================================================

    TYPED_TEST(PhysicsCoreTest, FreeFallFollowsGravity)
    {
        const auto ball = this->SpawnBall("Faller", Vector3(0.0f, 100.0f, 0.0f));
        this->Step(60);

        // One second of semi-implicit Euler at 1/60 s: y = 100 - g * dt^2 * n(n+1)/2 = 95.01 (the exact fall is 95.095)
        EXPECT_NEAR(ball->GetPositionable()->GetPosition().y, 95.01f, 0.2f);
        EXPECT_NEAR(ball->template GetComponent<Rigidbody>()->GetVelocity().y, -9.81f, 0.2f);
    }

    TYPED_TEST(PhysicsCoreTest, BodyWithoutCollidersFalls)
    {
        const auto go = this->Spawn("Shapeless", Vector3(0.0f, 50.0f, 0.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        this->Step(30);

        EXPECT_LT(go->GetPositionable()->GetPosition().y, 49.0f);
    }

    TYPED_TEST(PhysicsCoreTest, GravityDisabledBodyStaysPut)
    {
        const auto ball = this->SpawnBall("Floater", Vector3(1.0f, 7.0f, 2.0f));
        ball->template GetComponent<Rigidbody>()->SetGravityEnabled(false);
        this->Step(30);

        ExpectVectorNear(ball->GetPositionable()->GetPosition(), Vector3(1.0f, 7.0f, 2.0f), 1e-4f);
    }

    TYPED_TEST(PhysicsCoreTest, SetGravityChangesTheDirectionOfFall)
    {
        const auto ball = this->SpawnBall("Sideways", Vector3(0.0f, 0.0f, 0.0f));
        EXPECT_EQ(this->BackendRef().GetGravity().y, -9.81f);

        // Deferred to the next step, which also creates the body: it never falls along y
        this->BackendRef().SetGravity(Vector3(0.0f, 0.0f, -9.81f));
        this->Step(30);

        EXPECT_FLOAT_EQ(this->BackendRef().GetGravity().z, -9.81f);
        const Vector3 position = ball->GetPositionable()->GetPosition();
        EXPECT_LT(position.z, -1.0f);
        EXPECT_NEAR(position.y, 0.0f, 0.05f);
    }

    TYPED_TEST(PhysicsCoreTest, ImpulseChangesVelocityByImpulseOverMass)
    {
        const auto go = this->Spawn("Pushed", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<SphereCollider>()->SetRadius(0.5f);
        auto *body = this->AddFloatingBody(*go, 2.0f);
        this->Step(1);

        body->AddImpulse(Vector3(4.0f, 0.0f, 0.0f));
        this->Step(1);

        EXPECT_NEAR(body->GetVelocity().x, 2.0f, 0.05f);
    }

    TYPED_TEST(PhysicsCoreTest, ForceActsForOneStepOnly)
    {
        const auto go = this->Spawn("Pulled", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<SphereCollider>()->SetRadius(0.5f);
        auto *body = this->AddFloatingBody(*go, 2.0f);
        this->Step(1);

        body->AddForce(Vector3(0.0f, 10.0f, 0.0f));
        this->Step(1);
        const float afterOneStep = body->GetVelocity().y;
        // dv = F / m * dt = 10 / 2 / 60
        EXPECT_NEAR(afterOneStep, 10.0f / 2.0f / 60.0f, 0.01f);

        this->Step(5);
        EXPECT_NEAR(body->GetVelocity().y, afterOneStep, 1e-3f) << "the force was applied again in later steps";
    }

    TYPED_TEST(PhysicsCoreTest, SetVelocityMovesTheBodyAtThatVelocity)
    {
        const auto go = this->Spawn("Cruiser", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<SphereCollider>()->SetRadius(0.5f);
        auto *body = this->AddFloatingBody(*go, 1.0f);
        this->Step(1);

        body->SetVelocity(Vector3(3.0f, 0.0f, 0.0f));
        EXPECT_NEAR(body->GetVelocity().x, 3.0f, 1e-4f);
        this->Step(60);

        EXPECT_NEAR(go->GetPositionable()->GetPosition().x, 3.0f, 0.1f);
        EXPECT_NEAR(body->GetVelocity().x, 3.0f, 0.05f);
    }

    TYPED_TEST(PhysicsCoreTest, SetAngularVelocityTurnsTheBody)
    {
        const auto go = this->Spawn("Spinner", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *body = this->AddFloatingBody(*go, 1.0f);
        this->Step(1);

        body->SetAngularVelocity(Vector3(0.0f, 2.0f, 0.0f));
        EXPECT_NEAR(body->GetAngularVelocity().y, 2.0f, 1e-4f);
        this->Step(30);

        // Angular damping is 0.05 on both backends: about 2 rad/s kept, so a turn of about 1 rad about Y
        EXPECT_NEAR(body->GetAngularVelocity().y, 2.0f, 0.1f);
        EXPECT_GT(std::abs(go->GetPositionable()->GetRotation().GetY()), 0.3f);
    }

    // ============================================================================
    // Contact with a floor, materials
    // ============================================================================

    TYPED_TEST(PhysicsCoreTest, BallComesToRestOnAStaticFloor)
    {
        this->SpawnFloor();
        const auto ball = this->SpawnBall("Dropped", Vector3(0.0f, 3.0f, 0.0f));
        this->Step(240);

        // The floor's top is at y = 0.5 and the ball's radius 0.5; the engines' contact tolerances differ (Jolt's
        // penetration slop is 0.02), so a few centimetres either way
        const Vector3 position = ball->GetPositionable()->GetPosition();
        EXPECT_NEAR(position.y, 1.0f, 0.06f);
        EXPECT_NEAR(position.x, 0.0f, 0.01f);
        EXPECT_NEAR(position.z, 0.0f, 0.01f);
        EXPECT_LT(ball->template GetComponent<Rigidbody>()->GetVelocity().Length(), 0.2f);
    }

    TYPED_TEST(PhysicsCoreTest, BallComesToRestOnAStaticRigidbody)
    {
        const auto floor = this->Spawn("StaticFloor", Vector3(0.0f, 0.0f, 0.0f));
        floor->template AddComponent<BoxCollider>()->SetSize(Vector3(10.0f, 1.0f, 10.0f));
        floor->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Static);
        const auto ball = this->SpawnBall("Dropped", Vector3(0.0f, 3.0f, 0.0f));
        this->Step(240);

        EXPECT_NEAR(ball->GetPositionable()->GetPosition().y, 1.0f, 0.06f);
    }

    TYPED_TEST(PhysicsCoreTest, MovedStaticColliderMovesTheFloor)
    {
        const auto floor = this->SpawnFloor();
        this->Step(1);
        floor->GetPositionable()->SetPosition(Vector3(0.0f, -2.0f, 0.0f));
        const auto ball = this->SpawnBall("Dropped", Vector3(0.0f, 3.0f, 0.0f));
        this->Step(300);

        // The floor's top is now at y = -1.5
        EXPECT_NEAR(ball->GetPositionable()->GetPosition().y, -1.0f, 0.06f);
    }

    TYPED_TEST(PhysicsCoreTest, CompoundBodyRestsOnItsLowestCollider)
    {
        this->SpawnFloor();
        const auto go = this->Spawn("Tower", Vector3(0.0f, 4.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *lower = go->template AddComponent<BoxCollider>();
        lower->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        lower->SetOffset(Vector3(0.0f, -1.0f, 0.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Dynamic);
        this->Step(240);

        // The lower box's bottom is 1.5 below the object's origin; the floor's top is at 0.5
        EXPECT_NEAR(go->GetPositionable()->GetPosition().y, 2.0f, 0.06f);
        EXPECT_EQ(this->Native().GetShapeCount(body->GetHandle()), 2u);
    }

    TYPED_TEST(PhysicsCoreTest, MoreFrictionStopsASlidingBoxSooner)
    {
        const auto floor = this->Spawn("Floor", Vector3(0.0f, 0.0f, 0.0f));
        floor->template AddComponent<BoxCollider>()->SetSize(Vector3(40.0f, 1.0f, 40.0f));

        // Flat boxes (1 x 0.5 x 1, resting at y = 0.75): a cube would tip over when it slides on a grippy surface
        const auto slippery = this->Spawn("Ice", Vector3(0.0f, 0.8f, -3.0f));
        auto *iceCollider = slippery->template AddComponent<BoxCollider>();
        iceCollider->SetSize(Vector3(1.0f, 0.5f, 1.0f));
        iceCollider->SetMaterial(PhysicsMaterial::Ice());
        slippery->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Dynamic);

        const auto grippy = this->Spawn("Rubber", Vector3(0.0f, 0.8f, 3.0f));
        auto *rubberCollider = grippy->template AddComponent<BoxCollider>();
        rubberCollider->SetSize(Vector3(1.0f, 0.5f, 1.0f));
        rubberCollider->SetMaterial(PhysicsMaterial::Rubber());
        grippy->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Dynamic);

        this->Step(30); // settles on the floor
        slippery->template GetComponent<Rigidbody>()->SetVelocity(Vector3(6.0f, 0.0f, 0.0f));
        grippy->template GetComponent<Rigidbody>()->SetVelocity(Vector3(6.0f, 0.0f, 0.0f));
        this->Step(60);

        // The backends differ in how friction is described (PhysX has static and dynamic coefficients, Jolt one) and in
        // how it is applied, so only the order is compared, with a wide margin
        const float iceDistance = slippery->GetPositionable()->GetPosition().x;
        const float rubberDistance = grippy->GetPositionable()->GetPosition().x;
        EXPECT_GT(iceDistance, rubberDistance + 1.0f) << "ice " << iceDistance << ", rubber " << rubberDistance;
        EXPECT_GT(rubberDistance, 0.5f);
    }

    // ============================================================================
    // Inactive objects
    // ============================================================================

    TYPED_TEST(PhysicsCoreTest, InactiveBodyDoesNotSimulateAndResumesAtItsObjectsPose)
    {
        const auto ball = this->SpawnBall("Sleeper", Vector3(0.0f, 10.0f, 0.0f));
        this->Step(1);
        ball->SetActive(false);
        const float heightWhenDeactivated = ball->GetPositionable()->GetPosition().y;

        this->Step(30);
        EXPECT_FLOAT_EQ(ball->GetPositionable()->GetPosition().y, heightWhenDeactivated)
            << "an inactive body kept falling";

        // Moved while out of the simulation: it carries on from there, not from where it was taken out
        ball->GetPositionable()->SetPosition(Vector3(3.0f, 20.0f, 0.0f));
        ball->SetActive(true);
        this->Step(1);

        const Vector3 resumed = ball->GetPositionable()->GetPosition();
        EXPECT_NEAR(resumed.x, 3.0f, 1e-3f);
        EXPECT_GT(resumed.y, 19.0f);
        EXPECT_LT(resumed.y, 20.0f) << "it should be falling again";
    }

    // ============================================================================
    // Collider shapes
    // ============================================================================

    TYPED_TEST(PhysicsCoreTest, ColliderThenRigidbodyGivesOneShape)
    {
        const auto go = this->Spawn("ColliderFirst", Vector3(0.0f, 0.0f, 0.0f));
        auto *box = go->template AddComponent<BoxCollider>();
        box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Kinematic);
        this->Step(1);

        EXPECT_EQ(this->Native().GetShapeCount(body->GetHandle()), 1u);

        box->SetSize(Vector3(2.0f, 2.0f, 2.0f)); // used a shape the old static body had freed
        this->Step(1);
        EXPECT_EQ(this->Native().GetShapeCount(body->GetHandle()), 1u);
    }

    TYPED_TEST(PhysicsCoreTest, RigidbodyThenColliderGivesOneShape)
    {
        const auto go = this->Spawn("RigidbodyFirst", Vector3(0.0f, 0.0f, 0.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Kinematic);
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        this->Step(1);

        EXPECT_EQ(this->Native().GetShapeCount(body->GetHandle()), 1u) << "the shape was attached twice";
    }

    TYPED_TEST(PhysicsCoreTest, ChangingBodyTypeKeepsOneShape)
    {
        const auto go = this->Spawn("Retyped", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Kinematic);
        this->Step(1);

        body->SetBodyType(BodyType::Static); // recreates the body
        this->Step(1);

        EXPECT_EQ(this->Native().GetShapeCount(body->GetHandle()), 1u);
    }

    TYPED_TEST(PhysicsCoreTest, RemovingColliderRemovesItsShape)
    {
        const auto go = this->Spawn("Stripped", Vector3(0.0f, 0.0f, 0.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Kinematic);
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        this->Step(1);
        ASSERT_EQ(this->Native().GetShapeCount(body->GetHandle()), 1u);

        go->template RemoveComponent<BoxCollider>();
        this->Step(1);

        EXPECT_EQ(this->Native().GetShapeCount(body->GetHandle()), 0u) << "the shape stayed on the body as a ghost";
    }

    // ============================================================================
    // Transforms
    // ============================================================================

    TYPED_TEST(PhysicsCoreTest, KinematicBodyFollowsLatestTransform)
    {
        const auto go = this->Spawn("Mover", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *body = go->template AddComponent<Rigidbody>();
        body->SetBodyType(BodyType::Kinematic);
        this->Step(1);

        // Two moves before anything reads the transform: the second one used to be dropped, and the
        // first reached physics as the previous (stale) pose
        go->GetPositionable()->SetPosition(Vector3(1.0f, 0.0f, 0.0f));
        go->GetPositionable()->SetPosition(Vector3(3.0f, 0.0f, 0.0f));
        this->Step(1);

        const Vector3 physicsPos = this->BackendRef().GetPosition(body->GetHandle());
        EXPECT_NEAR(physicsPos.x, 3.0f, 1e-4f);
        EXPECT_NEAR(go->GetPositionable()->GetPosition().x, 3.0f, 1e-4f)
            << "the physics pose overwrote the script's move";
    }

    TYPED_TEST(PhysicsCoreTest, KinematicBodyStopsWhenItsObjectStopsMoving)
    {
        const auto go = this->Spawn("Mover", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        go->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
        auto *body = go->template GetComponent<Rigidbody>();
        this->Step(1);

        go->GetPositionable()->SetPosition(Vector3(2.0f, 0.0f, 0.0f));
        this->Step(1);
        const float reached = this->BackendRef().GetPosition(body->GetHandle()).x;
        this->Step(30);

        EXPECT_NEAR(this->BackendRef().GetPosition(body->GetHandle()).x, reached, 1e-4f)
            << "the kinematic body kept the velocity of its last move";
    }

    TYPED_TEST(PhysicsCoreTest, KinematicBodyPushesADynamicBody)
    {
        this->BackendRef().SetGravity(Vector3(0.0f, 0.0f, 0.0f));
        const auto wall = this->Spawn("Wall", Vector3(0.0f, 0.0f, 0.0f));
        wall->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 4.0f, 4.0f));
        wall->template AddComponent<Rigidbody>()->SetBodyType(BodyType::Kinematic);
        const auto ball = this->SpawnBall("Pushed", Vector3(2.0f, 0.0f, 0.0f));
        this->Step(2);

        for (int i = 1; i <= 40; ++i)
        {
            wall->GetPositionable()->SetPosition(Vector3(0.1f * static_cast<float>(i), 0.0f, 0.0f));
            this->Step(1);
        }

        // The wall is at x = 4 and has reached the ball (at 2) after 10 steps; a ball that was not pushed would still
        // be at x = 2
        EXPECT_GT(ball->GetPositionable()->GetPosition().x, 4.0f);
    }

    TYPED_TEST(PhysicsCoreTest, SceneSwitchReleasesDynamicBodies)
    {
        // The review's scenario: switch scenes with a dynamic body, then run a fixed step
        const auto ball = this->SpawnBall("Orphan", Vector3(0.0f, 10.0f, 0.0f));
        this->Step(1);
        const float heightAtSwitch = ball->GetPositionable()->GetPosition().y;

        SceneManager::AddScene(Scene::Create("PhysicsCore_AfterSwitch"), true);
        SceneManager::ProcessAnyPendingSceneChange();
        this->_scene = SceneManager::GetCurScene();
        this->Step(30); // SyncTransforms used to reach the old scene's Rigidbody

        EXPECT_FLOAT_EQ(ball->GetPositionable()->GetPosition().y, heightAtSwitch)
            << "the old scene's body kept simulating";
        EXPECT_FALSE(ball->template GetComponent<Rigidbody>()->GetHandle().IsValid());
    }

    // ============================================================================
    // Mass properties (inertia and centre of mass follow the colliders; the mass is the Rigidbody's)
    // ============================================================================

    TYPED_TEST(PhysicsCoreTest, BodyWithoutCollidersKeepsItsMass)
    {
        const auto go = this->Spawn("Shapeless", Vector3(0.0f, 0.0f, 0.0f));
        auto *body = this->AddFloatingBody(*go, 3.0f);
        this->Step(1);

        // Fallback: the configured mass, a centred COM, and inertia that scales with the mass (PhysX's own
        // shapeless default is (1,1,1) whatever the mass)
        EXPECT_NEAR(body->GetMass(), 3.0f, 1e-4f);
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()), Vector3(3.0f, 3.0f, 3.0f), 1e-4f);
    }

    TYPED_TEST(PhysicsCoreTest, InertiaAndCenterFollowBoxSizeAndOffset)
    {
        const auto go = this->Spawn("Plank", Vector3(0.0f, 0.0f, 0.0f));
        auto *box = go->template AddComponent<BoxCollider>();
        box->SetSize(Vector3(1.0f, 2.0f, 3.0f));
        auto *body = this->AddFloatingBody(*go, 2.0f);
        this->Step(1);

        EXPECT_NEAR(body->GetMass(), 2.0f, 1e-4f) << "the collider's volume must not override the Rigidbody's mass";
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(2.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);

        box->SetOffset(Vector3(0.0f, 1.0f, 0.0f));
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 1.0f, 0.0f), 1e-4f);

        box->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(2.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);
        EXPECT_NEAR(body->GetMass(), 2.0f, 1e-4f);
    }

    TYPED_TEST(PhysicsCoreTest, ChangingMassScalesInertia)
    {
        const auto go = this->Spawn("Scaled", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 2.0f, 3.0f));
        auto *body = this->AddFloatingBody(*go, 2.0f);
        this->Step(1);

        body->SetMass(4.0f);

        EXPECT_NEAR(body->GetMass(), 4.0f, 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(4.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);
    }

    TYPED_TEST(PhysicsCoreTest, LongBoxHasLessInertiaAboutItsLongAxis)
    {
        // Equal masses: the rod spins most easily about its long axis, the cube the same about every axis
        const auto rod = this->Spawn("Rod", Vector3(0.0f, 0.0f, 0.0f));
        rod->template AddComponent<BoxCollider>()->SetSize(Vector3(4.0f, 0.5f, 0.5f));
        auto *rodBody = this->AddFloatingBody(*rod, 1.0f);
        const auto cube = this->Spawn("Cube", Vector3(0.0f, 5.0f, 0.0f));
        cube->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *cubeBody = this->AddFloatingBody(*cube, 1.0f);
        this->Step(1);

        const Vector3 rodInertia = this->Native().GetInertiaTensor(rodBody->GetHandle());
        const Vector3 cubeInertia = this->Native().GetInertiaTensor(cubeBody->GetHandle());
        EXPECT_LT(rodInertia.x, cubeInertia.x);
        EXPECT_GT(rodInertia.y, cubeInertia.y);
        EXPECT_LT(rodInertia.x * 10.0f, rodInertia.y) << "inertia isn't following the collider's shape";
    }

    TYPED_TEST(PhysicsCoreTest, CapsuleHasLeastInertiaAboutItsAxis)
    {
        // The principal moments may be reported in any order: compare them sorted. Two are equal (across the axis),
        // the one about the axis is much smaller.
        const auto go = this->Spawn("Pill", Vector3(0.0f, 0.0f, 0.0f));
        auto *capsule = go->template AddComponent<CapsuleCollider>();
        capsule->SetRadius(0.25f);
        capsule->SetHeight(2.0f);
        capsule->SetOffset(Vector3(0.0f, 0.5f, 0.0f));
        auto *body = this->AddFloatingBody(*go, 1.0f);
        this->Step(1);

        const Vector3 inertia = this->Native().GetInertiaTensor(body->GetHandle());
        std::array<float, 3> moments{inertia.x, inertia.y, inertia.z};
        std::ranges::sort(moments);
        EXPECT_GT(moments[0], 0.0f);
        EXPECT_LT(moments[0] * 4.0f, moments[1]);
        EXPECT_NEAR(moments[1], moments[2], moments[2] * 1e-3f);
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.5f, 0.0f), 1e-4f);
    }

    TYPED_TEST(PhysicsCoreTest, InvalidDeserializedMassFallsBackToOne)
    {
        const auto go = this->Spawn("Massless", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *body = this->AddFloatingBody(*go, 2.0f);
        nlohmann::json data;
        data["_mass"] = 0.0f;
        body->Deserialize(data, nullptr); // unlike SetMass, doesn't clamp
        this->Step(1);

        EXPECT_NEAR(body->GetMass(), 1.0f, 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(1.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);

        body->AddImpulse(Vector3(1.0f, 0.0f, 0.0f));
        this->Step(1);
        EXPECT_NEAR(body->GetVelocity().x, 1.0f, 0.05f) << "a zero mass made the dynamic body immovable";
    }

    TYPED_TEST(PhysicsCoreTest, OffsetColliderMovesCenterOfMass)
    {
        const auto go = this->Spawn("Compound", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *body = this->AddFloatingBody(*go, 1.0f);
        this->Step(1);
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);

        auto *sphere = go->template AddComponent<SphereCollider>();
        sphere->SetRadius(0.5f);
        sphere->SetOffset(Vector3(2.0f, 0.0f, 0.0f));
        this->Step(1);

        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(CubeAndSphereCenterX, 0.0f, 0.0f),
                         1e-3f);
        EXPECT_NEAR(body->GetMass(), 1.0f, 1e-4f);

        ASSERT_TRUE(go->template RemoveComponent<SphereCollider>());
        this->Step(1);
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(1.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);
    }

    TYPED_TEST(PhysicsCoreTest, TriggerColliderDoesNotAffectMassProperties)
    {
        const auto go = this->Spawn("Sensored", Vector3(0.0f, 0.0f, 0.0f));
        go->template AddComponent<BoxCollider>()->SetSize(Vector3(1.0f, 1.0f, 1.0f));
        auto *sensor = go->template AddComponent<SphereCollider>();
        sensor->SetRadius(0.5f);
        sensor->SetOffset(Vector3(2.0f, 0.0f, 0.0f));
        sensor->SetIsTrigger(true);
        auto *body = this->AddFloatingBody(*go, 1.0f);
        this->Step(1);

        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.0f, 0.0f), 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(1.0f, Vector3(1.0f, 1.0f, 1.0f)), 1e-3f);

        sensor->SetIsTrigger(false); // now solid, so it counts
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(CubeAndSphereCenterX, 0.0f, 0.0f),
                         1e-3f);
    }

    TYPED_TEST(PhysicsCoreTest, ChangingBodyTypeKeepsMassProperties)
    {
        const auto go = this->Spawn("Retyped", Vector3(0.0f, 0.0f, 0.0f));
        auto *box = go->template AddComponent<BoxCollider>();
        box->SetSize(Vector3(1.0f, 2.0f, 3.0f));
        box->SetOffset(Vector3(0.0f, 0.5f, 0.0f));
        auto *body = this->AddFloatingBody(*go, 2.0f);
        this->Step(1);

        body->SetBodyType(BodyType::Kinematic); // recreates the body
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.5f, 0.0f), 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(2.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);

        body->SetBodyType(BodyType::Dynamic);
        this->Step(1);

        EXPECT_NEAR(body->GetMass(), 2.0f, 1e-4f);
        ExpectVectorNear(this->Native().GetCenterOfMass(body->GetHandle()), Vector3(0.0f, 0.5f, 0.0f), 1e-4f);
        ExpectVectorNear(this->Native().GetInertiaTensor(body->GetHandle()),
                         BoxInertia(2.0f, Vector3(1.0f, 2.0f, 3.0f)), 1e-3f);
    }
}

#endif // N2_PHYSICS_TEST_BACKENDS
