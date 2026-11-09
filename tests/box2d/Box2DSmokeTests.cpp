// The vendored Box2D alone: it runs, it is single-threaded as the engine uses it, and it gives the same result on every
// CI leg. The hashes are hard-coded on purpose: a leg that differs fails here, which is the only proof the project has for
// MSVC, arm64 and the web. Do not loosen a golden to make a leg pass; a difference is a finding (docs/physics.html#box2d).

#include <gtest/gtest.h>

#include <box2d/box2d.h>
#include <determinism.h>

#include <cstdint>
#include <cstdio>

namespace
{
    // The state hash (b2World_GetStateHash) of upstream's Falling Hinges scene after 100 steps of the engine's fixed step,
    // while its bodies are still moving. Measured with Box2D v3.2.0 on wasm and native x86-64 before vendoring.
    constexpr std::uint64_t kGoldenStateHash50Hz = 0x382FECF9034E1157ull;
    constexpr int kSteps50Hz = 100;
    constexpr float kEngineFixedStep = 0.02f;
    constexpr int kSubSteps = 4;

    b2WorldId MakeSingleThreadedWorld()
    {
        b2WorldDef def = b2DefaultWorldDef();
        def.workerCount = 1;
        return b2CreateWorld(&def);
    }

    struct Run50Hz
    {
        std::uint64_t hash = 0;
        int moving = 0;
    };

    Run50Hz RunFallingHinges50Hz()
    {
        const b2WorldId world = MakeSingleThreadedWorld();
        FallingHingeData data = CreateFallingHinges(world);
        for (int i = 0; i < kSteps50Hz; ++i)
        {
            b2World_Step(world, kEngineFixedStep, kSubSteps);
        }
        Run50Hz result;
        result.moving = b2World_GetBodyEvents(world).moveCount;
        result.hash = b2World_GetStateHash(world);
        DestroyFallingHinges(&data);
        b2DestroyWorld(world);
        return result;
    }
}

TEST(Box2DSmoke, WorkerCountOneMeansNoInternalThreads)
{
    const b2WorldId world = MakeSingleThreadedWorld();
    EXPECT_EQ(b2World_GetWorkerCount(world), 1);
    b2DestroyWorld(world);
}

TEST(Box2DSmoke, BoxComesToRestOnTheFloor)
{
    const b2WorldId world = MakeSingleThreadedWorld();

    const b2BodyDef floorDef = b2DefaultBodyDef();
    const b2BodyId floor = b2CreateBody(world, &floorDef);
    const b2ShapeDef shapeDef = b2DefaultShapeDef();
    const b2Polygon floorBox = b2MakeBox(50.0f, 1.0f); // top at y = 1
    b2CreatePolygonShape(floor, &shapeDef, &floorBox);

    b2BodyDef boxDef = b2DefaultBodyDef();
    boxDef.type = b2_dynamicBody;
    boxDef.position = b2Pos{0.0f, 5.0f};
    const b2BodyId box = b2CreateBody(world, &boxDef);
    const b2Polygon boxShape = b2MakeBox(0.5f, 0.5f);
    b2CreatePolygonShape(box, &shapeDef, &boxShape);

    for (int i = 0; i < 300; ++i)
    {
        b2World_Step(world, kEngineFixedStep, kSubSteps);
    }

    EXPECT_NEAR(b2Body_GetPosition(box).y, 1.5f, 0.02f);
    EXPECT_FALSE(b2Body_IsAwake(box));
    b2DestroyWorld(world);
}

// Upstream's own golden, driven as its test/test_determinism.c drives it
TEST(Box2DSmoke, FallingHingesMatchesUpstreamGolden)
{
    const b2WorldId world = MakeSingleThreadedWorld();
    FallingHingeData data = CreateFallingHinges(world);
    for (int i = 0; i < 500; ++i)
    {
        b2World_Step(world, 1.0f / 60.0f, kSubSteps);
        if (UpdateFallingHinges(world, &data))
        {
            break;
        }
    }
    const std::uint32_t hash = data.hash;
    const int sleepStep = data.sleepStep;
    DestroyFallingHinges(&data);
    b2DestroyWorld(world);

    EXPECT_EQ(sleepStep, EXPECTED_SLEEP_STEP);
    EXPECT_EQ(hash, static_cast<std::uint32_t>(EXPECTED_HASH));
}

TEST(Box2DSmoke, StateHashEqualsTheGoldenOnEveryLeg)
{
    const Run50Hz first = RunFallingHinges50Hz();
    const Run50Hz second = RunFallingHinges50Hz();

    std::printf("[BOX2D-DETERMINISM] steps=%d moving=%d hash=0x%016llx golden=0x%016llx\n", kSteps50Hz, first.moving,
                static_cast<unsigned long long>(first.hash), static_cast<unsigned long long>(kGoldenStateHash50Hz));

    EXPECT_GT(first.moving, 0) << "the scene must still be moving, or the hash proves little";
    EXPECT_EQ(first.hash, second.hash) << "two runs in one process differ";
    EXPECT_EQ(first.hash, kGoldenStateHash50Hz);
}
