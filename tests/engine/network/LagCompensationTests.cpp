#include "engine/network/LagCompensation.hpp"
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
using namespace N2Engine;
using namespace N2Engine::Network;

TEST(LagCompensationTest, InterpolatesAndClampsRewindToServerMeasuredRtt)
{
    LagCompensation history;
    std::array<HistoricalHitbox, 1> boxes;
    boxes[0].object = NetworkId{1};
    boxes[0].centre = Math::Vector3{0, 0, 5};
    ASSERT_TRUE(history.Capture(100'000, boxes));
    boxes[0].centre = Math::Vector3{0, 0, 9};
    ASSERT_TRUE(history.Capture(200'000, boxes));
    const auto hit = history.Cast(200'000, 0, 50'000, {0, 0, 0}, {0, 0, 2}, 20);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->timeMicros, 150'000u);
    EXPECT_NEAR(hit->distance, 6.5f, 0.001f);
    const auto current = history.Cast(200'000, 0, 0, {0, 0, 0}, {0, 0, 1}, 20);
    ASSERT_TRUE(current);
    EXPECT_NEAR(current->distance, 8.5f, 0.001f);
}
TEST(LagCompensationTest, SphereSweepHitsCapsuleAndRoundedBoxWithoutChangingHistory)
{
    for (auto shape : {HistoricalHitbox::Shape::Sphere, HistoricalHitbox::Shape::Capsule, HistoricalHitbox::Shape::Box})
    {
        LagCompensation history;
        std::array<HistoricalHitbox, 1> boxes;
        boxes[0].object = NetworkId{2};
        boxes[0].shape = shape;
        boxes[0].centre = Math::Vector3{0, 0, 5};
        boxes[0].endpoint = Math::Vector3{0, 2, 5};
        ASSERT_TRUE(history.Capture(10, boxes));
        auto hit = history.Cast(10, 10, 0, {0, 0, 0}, {0, 0, 1}, 20, 1);
        ASSERT_TRUE(hit);
        EXPECT_NEAR(hit->distance, 3.5f, 0.001f);
        EXPECT_FALSE(history.Cast(10, 10, 0, {5, 0, 0}, {0, 0, 1}, 20));
        EXPECT_EQ(history.FrameCount(), 1u);
    }
}
TEST(LagCompensationTest, BoundsValidationDespawnAndStableTies)
{
    LagCompensation history(200'000, 3);
    std::array<HistoricalHitbox, 2> boxes;
    boxes[0].object = NetworkId{2};
    boxes[1].object = NetworkId{1};
    boxes[0].centre = boxes[1].centre = Math::Vector3{0, 0, 5};
    for (std::uint64_t t = 0; t < 10; ++t)
        ASSERT_TRUE(history.Capture(t, boxes));
    EXPECT_EQ(history.FrameCount(), 3u);
    EXPECT_FALSE(history.Capture(0, boxes));
    auto hit = history.Cast(9, 9, 0, {0, 0, 0}, {0, 0, 1}, 20);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->object.value, 1u);
    history.Forget(NetworkId{1});
    hit = history.Cast(9, 9, 0, {0, 0, 0}, {0, 0, 1}, 20);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->object.value, 2u);
    EXPECT_FALSE(history.Cast(9, 9, 0, {0, 0, 0}, {0, 0, 0}, 20));
    boxes[0].radius = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(history.Capture(10, boxes));
    EXPECT_EQ(history.FrameCount(), 3u);
}

TEST(LagCompensationTest, LayersIgnoreInsideAndTeleportAreRespected)
{
    LagCompensation history;
    std::array<HistoricalHitbox, 1> boxes;
    boxes[0].object = NetworkId{1};
    boxes[0].centre = Math::Vector3{0, 0, 5};
    boxes[0].layers = 2;
    ASSERT_TRUE(history.Capture(100, boxes));
    EXPECT_FALSE(history.Cast(100, 100, 0, {0, 0, 0}, {0, 0, 1}, 20, 0, 1));
    EXPECT_FALSE(history.Cast(100, 100, 0, {0, 0, 0}, {0, 0, 1}, 20, 0, 2, NetworkId{1}));
    auto inside = history.Cast(100, 100, 0, {0, 0, 5}, {0, 0, 1}, 20);
    ASSERT_TRUE(inside);
    EXPECT_EQ(inside->distance, 0);
    boxes[0].centre = Math::Vector3{0, 0, 15};
    boxes[0].teleportSerial = 1;
    ASSERT_TRUE(history.Capture(200, boxes));
    const auto before = history.Cast(200, 150, 100, {0, 0, 0}, {0, 0, 1}, 20);
    ASSERT_TRUE(before);
    EXPECT_NEAR(before->distance, 4.5f, 0.001f);
    const auto after = history.Cast(200, 200, 100, {0, 0, 0}, {0, 0, 1}, 20);
    ASSERT_TRUE(after);
    EXPECT_NEAR(after->distance, 14.5f, 0.001f);
}

TEST(LagCompensationTest, RoundedBoxSweepDoesNotHitTheEmptyCornersOfAnInflatedBox)
{
    LagCompensation history;
    std::array<HistoricalHitbox, 1> boxes;
    boxes[0].object = NetworkId{1};
    boxes[0].shape = HistoricalHitbox::Shape::Box;
    boxes[0].centre = Math::Vector3{0, 0, 5};
    ASSERT_TRUE(history.Capture(10, boxes));
    EXPECT_FALSE(history.Cast(10, 10, 0, {1.4f, 1.4f, 0}, {0, 0, 1}, 20, 1));
    const auto corner = history.Cast(10, 10, 0, {1.1f, 1.1f, 0}, {0, 0, 1}, 20, 1);
    ASSERT_TRUE(corner);
    EXPECT_NEAR(corner->distance, 4.5f - std::sqrt(0.28f), 0.001f);
    const auto face = history.Cast(10, 10, 0, {0, 0, 0}, {0.05f, 0, 1}, 20);
    ASSERT_TRUE(face);
    EXPECT_NEAR(face->normal.z, -1, 0.001f);
    EXPECT_NEAR(face->normal.x, 0, 0.001f);
}

TEST(LagCompensationTest, OrientedBoxAndCapsuleEndcapsUseTheirActualGeometry)
{
    LagCompensation history;
    std::array<HistoricalHitbox,1> boxes;
    auto &box=boxes[0];
    box.object=NetworkId{1};box.shape=HistoricalHitbox::Shape::Box;
    box.centre=Math::Vector3{0,0,5};box.halfExtents=Math::Vector3{2,0.5f,0.5f};
    box.rotation=Math::Quaternion(Math::Vector3{0,1,0},std::numbers::pi_v<float>/2);
    ASSERT_TRUE(history.Capture(10,boxes));
    auto hit=history.Cast(10,10,0,{0,0,0},{0,0,1},20);
    ASSERT_TRUE(hit);EXPECT_NEAR(hit->distance,3,0.001f);EXPECT_NEAR(hit->normal.z,-1,0.001f);
    box.shape=HistoricalHitbox::Shape::Capsule;
    box.centre=Math::Vector3{0,-1,5};box.endpoint=Math::Vector3{0,1,5};
    ASSERT_TRUE(history.Capture(20,boxes));
    hit=history.Cast(20,20,0,{0,1.4f,0},{0,0,1},20);
    ASSERT_TRUE(hit);EXPECT_NEAR(hit->distance,4.7f,0.001f);
}
