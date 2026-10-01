#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

#include "renderer/common/RenderState.hpp"
#include "renderer/software/DrawOrder.hpp"

// The software renderer's draw ordering (OrderDraws), which is pure and needs no GL context

using Renderer::Common::RenderState;
using Renderer::Software::DrawOrderKey;
using Renderer::Software::IsReorderable;
using Renderer::Software::OrderDraws;

namespace
{
    DrawOrderKey Opaque(const float distanceSq)
    {
        return DrawOrderKey{RenderState::Opaque(), distanceSq};
    }

    DrawOrderKey Transparent(const float distanceSq)
    {
        return DrawOrderKey{RenderState::Transparent(), distanceSq};
    }

    std::vector<std::size_t> Order(const std::vector<DrawOrderKey> &draws)
    {
        return OrderDraws(draws);
    }
}

TEST(DrawOrderTest, EmptyFrameHasNoDraws)
{
    EXPECT_TRUE(Order({}).empty());
}

TEST(DrawOrderTest, OnlyDepthTestedDepthWritingDrawsAreReorderable)
{
    EXPECT_TRUE(IsReorderable(RenderState::Opaque()));
    EXPECT_FALSE(IsReorderable(RenderState::Transparent()));

    RenderState noDepthTest;
    noDepthTest.depthTest = false;
    EXPECT_FALSE(IsReorderable(noDepthTest));

    RenderState noCull;
    noCull.cull = Renderer::Common::CullMode::None;
    EXPECT_TRUE(IsReorderable(noCull));
}

TEST(DrawOrderTest, OpaqueDrawsSortFrontToBack)
{
    EXPECT_EQ(Order({Opaque(9.0f), Opaque(1.0f), Opaque(4.0f)}), (std::vector<std::size_t>{1, 2, 0}));
}

TEST(DrawOrderTest, EqualDistancesKeepSubmissionOrder)
{
    EXPECT_EQ(Order({Opaque(2.0f), Opaque(1.0f), Opaque(2.0f), Opaque(1.0f)}),
              (std::vector<std::size_t>{1, 3, 0, 2}));
}

TEST(DrawOrderTest, TransparentDrawsStayAfterOpaqueInSubmissionOrder)
{
    // What Scene::Render submits: opaque draws, then transparent ones already sorted back to front
    EXPECT_EQ(Order({Opaque(5.0f), Opaque(1.0f), Transparent(9.0f), Transparent(4.0f), Transparent(16.0f)}),
              (std::vector<std::size_t>{1, 0, 2, 3, 4}));
}

TEST(DrawOrderTest, NonReorderableDrawsKeepTheirPositions)
{
    RenderState overlay;
    overlay.depthTest = false;

    // Each run of reorderable draws is sorted on its own; nothing crosses a non-reorderable draw
    const std::vector<DrawOrderKey> draws{
        Opaque(5.0f), Opaque(1.0f), DrawOrderKey{overlay, 0.0f}, Opaque(3.0f), Opaque(2.0f), Transparent(0.0f),
        Opaque(9.0f), Opaque(8.0f)};
    EXPECT_EQ(Order(draws), (std::vector<std::size_t>{1, 0, 2, 4, 3, 5, 7, 6}));
}

TEST(DrawOrderTest, NanDistanceSortsLastInItsRun)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(Order({Opaque(nan), Opaque(3.0f), Opaque(nan), Opaque(1.0f)}),
              (std::vector<std::size_t>{3, 1, 0, 2}));
}
