// IPhysicsBackend::GetStats (EN5, the editor's Profiler): the body counts each backend reports, on every backend this build
// has. PhysX: PxScene::getNbActors and getSimulationStatistics; Jolt: PhysicsSystem::GetBodyStats.

#include "PhysicsTestFixture.hpp"

#ifdef N2_PHYSICS_TEST_BACKENDS

namespace N2PhysicsTests
{
    template <typename Tag>
    class PhysicsStatsTest : public PhysicsCoreFixture<Tag>
    {
    };

    TYPED_TEST_SUITE(PhysicsStatsTest, AllBackends, BackendNames);

    TYPED_TEST(PhysicsStatsTest, ABackendThatIsNotInitializedHasNoStats)
    {
        typename TypeParam::Backend backend;
        EXPECT_FALSE(backend.GetStats().has_value());
    }

    TYPED_TEST(PhysicsStatsTest, TheBodiesAreCountedWithTheStaticAndTheAwakeOnes)
    {
        // What earlier tests left is gone after a step (their scene was replaced in SetUp)
        this->Step();
        const std::optional<PhysicsStats> before = this->BackendRef().GetStats();
        ASSERT_TRUE(before.has_value());

        this->SpawnFloor();
        this->SpawnBall("A", Vector3(-1.0f, 3.0f, 0.0f));
        this->SpawnBall("B", Vector3(1.0f, 3.0f, 0.0f));
        // Two steps: whatever the attach order made and replaced in the first (a collider's own static body before its
        // Rigidbody's) is gone by the second
        this->Step(2);

        const std::optional<PhysicsStats> after = this->BackendRef().GetStats();
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->bodies, before->bodies + 3) << "a floor and two balls";
        EXPECT_EQ(after->staticBodies, before->staticBodies + 1) << "the floor";
        EXPECT_GE(after->activeBodies, 2u) << "the two falling balls are awake";
        EXPECT_LE(after->activeBodies, after->bodies - after->staticBodies);
    }
}

#endif
