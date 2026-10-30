#include <gtest/gtest.h>

#include "engine/Layers.hpp"

#ifdef N2ENGINE_PHYSX_ENABLED
#include "engine/physics/physx/PhysXLayerFilter.hpp"

// The PhysX scene's filter shader, called directly (no scene, no GPU), so every PhysX leg of CI guards what only the
// GPU golden test (PhysXGpuSoftBodiesGoldenTest) could see: a soft body's deformable volume against a floor collider.
// Its shape used to keep PhysX's all-zero filter data, which the layer check rejects against every layer, so on a GPU
// the bodies fell through the floor (lowest point -95.7 m after 4.5 s).

using namespace N2Engine;
using namespace N2Engine::Physics;

namespace
{
    class PhysXLayerFilterTest : public ::testing::Test
    {
    protected:
        void SetUp() override { Layers::ResetToDefaults(); }
        void TearDown() override { Layers::ResetToDefaults(); }

        static physx::PxFilterFlags Filter(const physx::PxFilterObjectType::Enum type0, const physx::PxFilterData &data0,
                                           const physx::PxFilterObjectType::Enum type1, const physx::PxFilterData &data1,
                                           physx::PxPairFlags &pairFlags)
        {
            return CollisionEventFilterShader(static_cast<physx::PxFilterObjectAttributes>(type0), data0,
                                              static_cast<physx::PxFilterObjectAttributes>(type1), data1, pairFlags,
                                              nullptr, 0);
        }
    };
}

TEST_F(PhysXLayerFilterTest, ASoftBodyTouchesAFloorOnTheDefaultLayer)
{
    physx::PxPairFlags pairFlags;
    const physx::PxFilterFlags flags = Filter(physx::PxFilterObjectType::eDEFORMABLE_VOLUME, UnlayeredSimulationData(),
                                              physx::PxFilterObjectType::eRIGID_STATIC,
                                              LayerSimulationData(Layers::Default), pairFlags);
    EXPECT_FALSE(flags.isSet(physx::PxFilterFlag::eSUPPRESS));
    EXPECT_FALSE(flags.isSet(physx::PxFilterFlag::eKILL));
    EXPECT_TRUE(pairFlags.isSet(physx::PxPairFlag::eSOLVE_CONTACT)) << "the floor must push the body back";
}

TEST_F(PhysXLayerFilterTest, ASoftBodyTouchesAColliderOnEveryLayerThatCollidesWithDefault)
{
    for (int layer = 0; layer < Layers::Count; ++layer)
    {
        physx::PxPairFlags pairFlags;
        const physx::PxFilterFlags flags = Filter(physx::PxFilterObjectType::eRIGID_DYNAMIC, LayerSimulationData(layer),
                                                  physx::PxFilterObjectType::eDEFORMABLE_VOLUME,
                                                  UnlayeredSimulationData(), pairFlags);
        EXPECT_EQ(flags.isSet(physx::PxFilterFlag::eSUPPRESS), !Layers::GetCollision(layer, Layers::Default))
            << "layer " << layer;
    }
}

TEST_F(PhysXLayerFilterTest, AllZeroFilterDataCollidesWithNothing)
{
    // Why the soft body's shape needs filter data at all: PhysX creates a shape with all-zero filter data
    physx::PxPairFlags pairFlags;
    const physx::PxFilterFlags flags = Filter(physx::PxFilterObjectType::eDEFORMABLE_VOLUME, physx::PxFilterData(),
                                              physx::PxFilterObjectType::eRIGID_STATIC,
                                              LayerSimulationData(Layers::Default), pairFlags);
    EXPECT_TRUE(flags.isSet(physx::PxFilterFlag::eSUPPRESS));
}

TEST_F(PhysXLayerFilterTest, TheMatrixKeepsALayerAwayFromSoftBodiesWithoutRefreshingThem)
{
    constexpr int ghosts = 8;
    const physx::PxFilterData soft = UnlayeredSimulationData(); // made before the matrix changes
    Layers::SetCollision(ghosts, Layers::Default, false);
    physx::PxPairFlags pairFlags;
    EXPECT_TRUE(Filter(physx::PxFilterObjectType::eDEFORMABLE_VOLUME, soft, physx::PxFilterObjectType::eRIGID_DYNAMIC,
                       LayerSimulationData(ghosts), pairFlags)
                    .isSet(physx::PxFilterFlag::eSUPPRESS));
    Layers::SetCollision(ghosts, Layers::Default, true);
    EXPECT_FALSE(Filter(physx::PxFilterObjectType::eDEFORMABLE_VOLUME, soft, physx::PxFilterObjectType::eRIGID_DYNAMIC,
                        LayerSimulationData(ghosts), pairFlags)
                     .isSet(physx::PxFilterFlag::eSUPPRESS));
}

TEST_F(PhysXLayerFilterTest, TriggersAndSolidPairsKeepTheirFlags)
{
    physx::PxPairFlags pairFlags;
    const auto trigger = static_cast<physx::PxFilterObjectAttributes>(physx::PxFilterObjectType::eRIGID_STATIC) |
                         physx::PxFilterObjectFlag::eTRIGGER;
    EXPECT_FALSE(CollisionEventFilterShader(trigger, LayerSimulationData(Layers::Default),
                                            physx::PxFilterObjectType::eRIGID_DYNAMIC,
                                            LayerSimulationData(Layers::Default), pairFlags, nullptr, 0)
                     .isSet(physx::PxFilterFlag::eSUPPRESS));
    EXPECT_EQ(pairFlags, physx::PxPairFlags(physx::PxPairFlag::eTRIGGER_DEFAULT));

    Filter(physx::PxFilterObjectType::eRIGID_DYNAMIC, LayerSimulationData(Layers::Default),
           physx::PxFilterObjectType::eRIGID_STATIC, LayerSimulationData(Layers::Default), pairFlags);
    EXPECT_TRUE(pairFlags.isSet(physx::PxPairFlag::eNOTIFY_TOUCH_FOUND));
    EXPECT_TRUE(pairFlags.isSet(physx::PxPairFlag::eNOTIFY_CONTACT_POINTS));
}

#else

TEST(PhysXLayerFilterTest, NeedsPhysX)
{
    GTEST_SKIP() << "this build has no PhysX";
}

#endif
