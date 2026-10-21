#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <math/Quaternion.hpp>
#include <math/Vector3.hpp>

#include "engine/physics/BodyState.hpp"
#include "engine/physics/physx/PhysXBackend.hpp"

// IPhysicsBackend::GetBodyState / SetBodyState on the PhysX backend: what each body kind takes, what is refused, and the
// round trip a client reconciliation relies on. PhysX only: a build without it has no backend to ask.
//
// No `using namespace N2Engine`: math/MathRegistrar.hpp forward-declares N2Engine::Vector3 and N2Engine::Quaternion,
// which would make the Math types ambiguous.

#ifdef N2ENGINE_PHYSX_ENABLED

using namespace N2Engine::Physics;
using N2Engine::Math::Quaternion;
using N2Engine::Math::Vector3;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    /// A backend with a ground box, so a body that is let go has something to land on. CPU scene, default settings.
    class BodyStateTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            ASSERT_TRUE(backend.Initialize());
        }

        PhysicsBodyHandle MakeBall(const Vector3 &position, const bool kinematic = false)
        {
            const PhysicsBodyHandle body = backend.CreateDynamicBody(position, Quaternion(), 1.0f, nullptr, kinematic);
            backend.AddSphereCollider(body, nullptr, 0.5f, Vector3::Zero, PhysicsMaterial{});
            return body;
        }

        PhysXBackend backend;
    };

    /// A rotation of `degrees` about the y axis, as (w, x, y, z)
    Quaternion AboutY(const float degrees)
    {
        const float half = degrees * 3.14159265358979f / 360.0f;
        return Quaternion(std::cos(half), 0.0f, std::sin(half), 0.0f);
    }

    /// Equal to float rounding: PhysX keeps a dynamic body's pose relative to its centre of mass, so a pose read back
    /// may differ from the one written in the last bit
    void ExpectSame(const BodyState &a, const BodyState &b)
    {
        constexpr float kTolerance = 1e-6f;
        EXPECT_NEAR(a.position.x, b.position.x, kTolerance);
        EXPECT_NEAR(a.position.y, b.position.y, kTolerance);
        EXPECT_NEAR(a.position.z, b.position.z, kTolerance);
        EXPECT_NEAR(a.rotation.GetW(), b.rotation.GetW(), kTolerance);
        EXPECT_NEAR(a.rotation.GetX(), b.rotation.GetX(), kTolerance);
        EXPECT_NEAR(a.rotation.GetY(), b.rotation.GetY(), kTolerance);
        EXPECT_NEAR(a.rotation.GetZ(), b.rotation.GetZ(), kTolerance);
        EXPECT_NEAR(a.linearVelocity.x, b.linearVelocity.x, kTolerance);
        EXPECT_NEAR(a.linearVelocity.y, b.linearVelocity.y, kTolerance);
        EXPECT_NEAR(a.linearVelocity.z, b.linearVelocity.z, kTolerance);
        EXPECT_NEAR(a.angularVelocity.x, b.angularVelocity.x, kTolerance);
        EXPECT_NEAR(a.angularVelocity.y, b.angularVelocity.y, kTolerance);
        EXPECT_NEAR(a.angularVelocity.z, b.angularVelocity.z, kTolerance);
        EXPECT_EQ(a.sleeping, b.sleeping);
    }
}

TEST_F(BodyStateTest, PhysXSaysItCanSetStateAndNamesItself)
{
    EXPECT_TRUE(backend.SupportsSetState());
    EXPECT_EQ(backend.BackendId(), "physx");
    // And that it cannot snapshot: the hooks stay unsupported and change nothing
    EXPECT_FALSE(backend.SupportsSnapshot());
    std::vector<std::byte> bytes{std::byte{7}};
    EXPECT_FALSE(backend.SaveState(bytes));
    EXPECT_EQ(bytes.size(), 1u);
    EXPECT_FALSE(backend.RestoreState(bytes));
    EXPECT_FALSE(backend.HashState().has_value());
}

TEST_F(BodyStateTest, ADynamicBodyIsTeleportedWithItsVelocitiesAndReadsBack)
{
    const PhysicsBodyHandle body = MakeBall(Vector3(0.0f, 10.0f, 0.0f));

    BodyState wanted;
    wanted.position = Vector3(3.0f, 4.5f, -2.25f);
    wanted.rotation = AboutY(90.0f);
    wanted.linearVelocity = Vector3(1.0f, -2.0f, 0.5f);
    wanted.angularVelocity = Vector3(0.25f, 0.5f, -0.75f);
    wanted.sleeping = false;
    ASSERT_TRUE(backend.SetBodyState(body, wanted));

    BodyState got;
    ASSERT_TRUE(backend.GetBodyState(body, got));
    ExpectSame(got, wanted);
    // The same numbers through the older accessors
    EXPECT_NEAR(backend.GetPosition(body).y, 4.5f, 1e-6f);
    EXPECT_NEAR(backend.GetVelocity(body).x, 1.0f, 1e-6f);
    EXPECT_NEAR(backend.GetAngularVelocity(body).z, -0.75f, 1e-6f);
}

TEST_F(BodyStateTest, ATeleportedBodyKeepsSimulatingFromTheNewState)
{
    const PhysicsBodyHandle body = MakeBall(Vector3(0.0f, 10.0f, 0.0f));
    BodyState wanted;
    wanted.position = Vector3(5.0f, 20.0f, 0.0f);
    wanted.linearVelocity = Vector3(6.0f, 0.0f, 0.0f);
    ASSERT_TRUE(backend.SetBodyState(body, wanted));

    backend.Update(kDt);
    BodyState after;
    ASSERT_TRUE(backend.GetBodyState(body, after));
    EXPECT_FALSE(after.sleeping);
    EXPECT_NEAR(after.position.x, 5.0f + 6.0f * kDt, 1e-3f) << "moved on from the teleport target";
    EXPECT_LT(after.position.y, 20.0f) << "and gravity pulls it down from there";
    EXPECT_GT(after.position.y, 19.9f);
    EXPECT_NEAR(after.linearVelocity.x, 6.0f, 5e-2f) << "less a little linear damping";
}

TEST_F(BodyStateTest, ARotationThatIsNotUnitLengthIsNormalised)
{
    const PhysicsBodyHandle body = MakeBall(Vector3(0.0f, 1.0f, 0.0f));
    BodyState wanted;
    wanted.position = Vector3(0.0f, 1.0f, 0.0f);
    wanted.rotation = Quaternion(2.0f, 0.0f, 0.0f, 0.0f); // w = 2: twice the identity
    ASSERT_TRUE(backend.SetBodyState(body, wanted));

    BodyState got;
    ASSERT_TRUE(backend.GetBodyState(body, got));
    EXPECT_NEAR(got.rotation.GetW(), 1.0f, 1e-6f);
    EXPECT_NEAR(got.rotation.GetX(), 0.0f, 1e-6f);
}

TEST_F(BodyStateTest, ASleepRequestPutsTheBodyToSleepWithNoVelocityAndAWakeRequestWakesIt)
{
    const PhysicsBodyHandle body = MakeBall(Vector3(0.0f, 10.0f, 0.0f));
    BodyState asleep;
    asleep.position = Vector3(1.0f, 10.0f, 2.0f);
    asleep.linearVelocity = Vector3(3.0f, 0.0f, 0.0f); // a sleeping body keeps none
    asleep.sleeping = true;
    ASSERT_TRUE(backend.SetBodyState(body, asleep));

    // PhysX applies the island change when the scene steps; the body must not move in that step
    backend.Update(kDt);
    BodyState got;
    ASSERT_TRUE(backend.GetBodyState(body, got));
    EXPECT_TRUE(got.sleeping);
    EXPECT_NEAR(got.position.x, 1.0f, 1e-6f);
    EXPECT_NEAR(got.position.y, 10.0f, 1e-6f);
    EXPECT_NEAR(got.position.z, 2.0f, 1e-6f);
    EXPECT_EQ(got.linearVelocity.x, 0.0f);

    BodyState awake = got;
    awake.sleeping = false;
    awake.linearVelocity = Vector3(0.0f, 0.0f, 4.0f);
    ASSERT_TRUE(backend.SetBodyState(body, awake));
    backend.Update(kDt);
    ASSERT_TRUE(backend.GetBodyState(body, got));
    EXPECT_FALSE(got.sleeping);
    EXPECT_GT(got.position.z, 2.0f) << "it moves again";
}

TEST_F(BodyStateTest, AKinematicBodyTakesOnlyThePoseAsATargetAndReportsNoVelocity)
{
    const PhysicsBodyHandle body = MakeBall(Vector3(0.0f, 5.0f, 0.0f), true);
    BodyState wanted;
    wanted.position = Vector3(2.0f, 6.0f, -1.0f);
    wanted.rotation = AboutY(45.0f);
    wanted.linearVelocity = Vector3(100.0f, 0.0f, 0.0f); // does not apply to a kinematic body
    wanted.sleeping = true;                              // nor does this
    ASSERT_TRUE(backend.SetBodyState(body, wanted));

    backend.Update(kDt);
    BodyState got;
    ASSERT_TRUE(backend.GetBodyState(body, got));
    EXPECT_NEAR(got.position.x, 2.0f, 1e-5f);
    EXPECT_NEAR(got.position.y, 6.0f, 1e-5f);
    EXPECT_NEAR(got.position.z, -1.0f, 1e-5f);
    EXPECT_NEAR(got.rotation.GetY(), wanted.rotation.GetY(), 1e-5f);
    EXPECT_EQ(got.linearVelocity.x, 0.0f);
    EXPECT_EQ(got.angularVelocity.y, 0.0f);
}

TEST_F(BodyStateTest, AStaticBodyTakesOnlyThePoseAndIsNeverAsleep)
{
    const PhysicsBodyHandle ground = backend.CreateStaticBody(Vector3(0.0f, -0.5f, 0.0f), Quaternion(), nullptr);
    backend.AddBoxCollider(ground, nullptr, Vector3(5.0f, 0.5f, 5.0f), Vector3::Zero, PhysicsMaterial{});

    BodyState wanted;
    wanted.position = Vector3(10.0f, -2.0f, 3.0f);
    wanted.linearVelocity = Vector3(9.0f, 9.0f, 9.0f);
    wanted.sleeping = true;
    ASSERT_TRUE(backend.SetBodyState(ground, wanted));

    BodyState got;
    ASSERT_TRUE(backend.GetBodyState(ground, got));
    EXPECT_EQ(got.position.x, 10.0f);
    EXPECT_EQ(got.position.y, -2.0f);
    EXPECT_EQ(got.position.z, 3.0f);
    EXPECT_EQ(got.linearVelocity.x, 0.0f);
    EXPECT_EQ(got.angularVelocity.x, 0.0f);
    EXPECT_FALSE(got.sleeping);
}

TEST_F(BodyStateTest, ABadStateIsRefusedAndChangesNothing)
{
    const PhysicsBodyHandle body = MakeBall(Vector3(1.0f, 2.0f, 3.0f));
    BodyState before;
    ASSERT_TRUE(backend.GetBodyState(body, before));

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
    noRotation.rotation = Quaternion(0.0f, 0.0f, 0.0f, 0.0f);
    BodyState nanRotation = good;
    nanRotation.rotation = Quaternion(nan, 0.0f, 0.0f, 0.0f);
    for (const BodyState *bad : {&badPosition, &badLinear, &badAngular, &noRotation, &nanRotation})
    {
        EXPECT_FALSE(backend.SetBodyState(body, *bad));
        BodyState now;
        ASSERT_TRUE(backend.GetBodyState(body, now));
        ExpectSame(now, before);
    }
}

TEST_F(BodyStateTest, AStaleInvalidOrDisabledBodyHasNoState)
{
    BodyState out;
    out.position = Vector3(7.0f, 7.0f, 7.0f);
    EXPECT_FALSE(backend.GetBodyState(INVALID_PHYSICS_HANDLE, out));
    EXPECT_FALSE(backend.SetBodyState(INVALID_PHYSICS_HANDLE, out));
    EXPECT_EQ(out.position.x, 7.0f) << "a refused read leaves the output alone";

    const PhysicsBodyHandle body = MakeBall(Vector3(0.0f, 3.0f, 0.0f));
    ASSERT_TRUE(backend.GetBodyState(body, out));

    // Out of the scene (a deactivated object): nothing to read or write
    backend.SetBodyEnabled(body, false);
    out.position = Vector3(7.0f, 7.0f, 7.0f);
    EXPECT_FALSE(backend.GetBodyState(body, out));
    EXPECT_EQ(out.position.x, 7.0f);
    EXPECT_FALSE(backend.SetBodyState(body, out));
    backend.SetBodyEnabled(body, true);
    EXPECT_TRUE(backend.GetBodyState(body, out));

    // Destroyed: the handle is stale
    backend.DestroyBody(body);
    EXPECT_FALSE(backend.GetBodyState(body, out));
    EXPECT_FALSE(backend.SetBodyState(body, out));
}

#endif // N2ENGINE_PHYSX_ENABLED
