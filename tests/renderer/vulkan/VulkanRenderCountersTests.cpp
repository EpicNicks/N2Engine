#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderCounters.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// IRenderer's render counters (EN5) in the Vulkan backend: each vkCmdDrawIndexed DrawLocked records is counted, a draw it
// skips is not, BeginFrame starts from zero and EndFrame publishes. On a Vulkan device (lavapipe in CI), validation on.

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::IndexRange;
using Renderer::Common::RenderCounters;
using Renderer::Common::RenderObject;

TEST_F(VulkanDrawTest, TheRenderCountersCountEachRecordedDrawAndNoSkippedOne)
{
    EXPECT_TRUE(R().SupportsRenderCounters());
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 0.0f, 0.0f, 0.0f)); // 4 vertices, 2 triangles
    IMesh* halves = MakeMesh(TwoHalves());                       // 8 vertices, 4 triangles
    ASSERT_NE(red, nullptr);
    ASSERT_NE(quad, nullptr);
    ASSERT_NE(halves, nullptr);
    const std::uint64_t framesBefore = R().GetLastFrameCounters().frames;

    (void)RenderFrame([&] {
        R().DrawMesh(quad, Identity, red);
        R().DrawMesh(halves, Identity, red);
        R().DrawMesh(halves, Identity, red, Renderer::Common::RenderState{}, IndexRange{6, 6}); // the right half
        R().DrawMesh(halves, Identity, red, Renderer::Common::RenderState{}, IndexRange{6, 12}); // past the end: skipped
    });
    RenderCounters counters = R().GetLastFrameCounters();
    EXPECT_EQ(counters.drawCalls, 3u);
    EXPECT_EQ(counters.triangles, 2u + 4u + 2u);
    EXPECT_EQ(counters.vertices, static_cast<std::uint64_t>(quad->GetVertexCount()) + 2u * halves->GetVertexCount());
    EXPECT_EQ(counters.frames, framesBefore + 1);

    std::vector<RenderObject> objects(2);
    for (RenderObject& object : objects)
    {
        object.mesh = quad;
        object.material = red;
        std::copy(std::begin(Identity), std::end(Identity), object.transform.model);
    }
    (void)RenderFrame([&] { R().DrawObjects(objects); });
    counters = R().GetLastFrameCounters();
    EXPECT_EQ(counters.drawCalls, 2u) << "the new frame starts from zero";
    EXPECT_EQ(counters.triangles, 4u);
    EXPECT_EQ(counters.frames, framesBefore + 2);
}
