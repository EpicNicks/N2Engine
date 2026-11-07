#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderCounters.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

// IRenderer's render counters (EN5, the editor's Profiler) in the software renderer: what a frame drew, counted as each
// draw is recorded (before the renderer's depth sort), cleared by BeginFrame and published by EndFrame.
namespace
{
    constexpr int Size = 16;
    constexpr float Identity[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    /// `quads` quads side by side: 4 vertices and 6 indices each
    MeshData Quads(const int quads, const float depth)
    {
        MeshData data;
        for (int q = 0; q < quads; ++q)
        {
            const auto base = static_cast<std::uint32_t>(data.vertices.size());
            const float x = -1.0f + 0.1f * static_cast<float>(q);
            data.vertices.push_back(Vertex{{x, -1, depth}, {0, 0, 1}, {0, 0}, {1, 1, 1, 1}});
            data.vertices.push_back(Vertex{{x + 0.1f, -1, depth}, {0, 0, 1}, {1, 0}, {1, 1, 1, 1}});
            data.vertices.push_back(Vertex{{x + 0.1f, 1, depth}, {0, 0, 1}, {1, 1}, {1, 1, 1, 1}});
            data.vertices.push_back(Vertex{{x, 1, depth}, {0, 0, 1}, {0, 1}, {1, 1, 1, 1}});
            for (const std::uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
            {
                data.indices.push_back(base + index);
            }
        }
        return data;
    }

    class SoftwareRenderCountersTest : public ::testing::Test
    {
    protected:
        SoftwareRenderer renderer;
        IMaterial *material = nullptr;
        IMesh *oneQuad = nullptr;   // 4 vertices, 2 triangles
        IMesh *threeQuads = nullptr; // 12 vertices, 6 triangles

        void SetUp() override
        {
            ASSERT_TRUE(renderer.Initialize(nullptr, Size, Size));
            material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
            oneQuad = renderer.CreateMesh(Quads(1, 0.5f));
            threeQuads = renderer.CreateMesh(Quads(3, -0.5f));
            ASSERT_NE(material, nullptr);
            ASSERT_NE(oneQuad, nullptr);
            ASSERT_NE(threeQuads, nullptr);
        }
        void TearDown() override { renderer.Shutdown(); }

        template <class Draw> RenderCounters Render(Draw draw)
        {
            renderer.BeginFrame();
            renderer.SetViewProjection(Identity, Identity);
            draw();
            renderer.EndFrame();
            return renderer.GetLastFrameCounters();
        }
    };
}

TEST(RenderCountersTest, TheFourBackendsCountAndTheInterfaceDefaultDoesNot)
{
    SoftwareRenderer renderer;
    EXPECT_TRUE(renderer.SupportsRenderCounters());
    const RenderCounters &none = renderer.GetLastFrameCounters();
    EXPECT_EQ(none.drawCalls, 0u) << "zeros before the first frame";
    EXPECT_EQ(none.frames, 0u);
}

TEST_F(SoftwareRenderCountersTest, EachDrawIsCountedWithItsTrianglesAndVertices)
{
    const RenderCounters counters = Render([&] {
        renderer.DrawMesh(oneQuad, Identity, material);
        renderer.DrawMesh(threeQuads, Identity, material);
        renderer.DrawMesh(oneQuad, Identity, material);
    });
    EXPECT_EQ(counters.drawCalls, 3u);
    EXPECT_EQ(counters.triangles, 2u + 6u + 2u);
    EXPECT_EQ(counters.vertices, 4u + 12u + 4u);
}

TEST_F(SoftwareRenderCountersTest, ARangeCountsItsIndicesClippedToTheMesh)
{
    RenderCounters counters = Render([&] {
        // The second quad of three: 6 indices from 6
        renderer.DrawMesh(threeQuads, Identity, material, RenderState{}, IndexRange{6, 6});
    });
    EXPECT_EQ(counters.drawCalls, 1u);
    EXPECT_EQ(counters.triangles, 2u);
    EXPECT_EQ(counters.vertices, 12u) << "the whole mesh's vertices are bound";

    counters = Render([&] {
        // 12 indices from 12: the software renderer clips to the 6 there are
        renderer.DrawMesh(threeQuads, Identity, material, RenderState{}, IndexRange{12, 12});
        // Past the end, and empty: nothing is drawn
        renderer.DrawMesh(threeQuads, Identity, material, RenderState{}, IndexRange{18, 6});
        renderer.DrawMesh(threeQuads, Identity, material, RenderState{}, IndexRange{0, 0});
    });
    EXPECT_EQ(counters.drawCalls, 1u);
    EXPECT_EQ(counters.triangles, 2u);
}

TEST_F(SoftwareRenderCountersTest, DrawObjectsCountsEachObject)
{
    std::vector<RenderObject> objects(2);
    for (RenderObject &object : objects)
    {
        object.mesh = threeQuads;
        object.material = material;
        std::copy(std::begin(Identity), std::end(Identity), object.transform.model);
    }
    const RenderCounters counters = Render([&] { renderer.DrawObjects(objects); });
    EXPECT_EQ(counters.drawCalls, 2u);
    EXPECT_EQ(counters.triangles, 12u);
    EXPECT_EQ(counters.vertices, 24u);
}

TEST_F(SoftwareRenderCountersTest, AFrameStartsFromZeroAndEndFramePublishesIt)
{
    const std::uint64_t framesBefore = renderer.GetLastFrameCounters().frames;
    (void)Render([&] {
        renderer.DrawMesh(threeQuads, Identity, material);
        renderer.DrawMesh(threeQuads, Identity, material);
    });
    EXPECT_EQ(renderer.GetLastFrameCounters().frames, framesBefore + 1);

    renderer.BeginFrame();
    renderer.DrawMesh(oneQuad, Identity, material);
    EXPECT_EQ(renderer.GetLastFrameCounters().drawCalls, 2u) << "the frame being drawn isn't published until EndFrame";
    renderer.EndFrame();
    EXPECT_EQ(renderer.GetLastFrameCounters().drawCalls, 1u) << "the new frame's own draws, from zero";
    EXPECT_EQ(renderer.GetLastFrameCounters().triangles, 2u);
    EXPECT_EQ(renderer.GetLastFrameCounters().frames, framesBefore + 2);

    const RenderCounters empty = Render([] {});
    EXPECT_EQ(empty.drawCalls, 0u);
    EXPECT_EQ(empty.triangles, 0u);
}

TEST_F(SoftwareRenderCountersTest, TheDepthSortDoesNotChangeTheCounts)
{
    // Depth-writing draws far then near: the renderer rasterizes them front to back, and counts them as recorded
    const RenderCounters counters = Render([&] {
        renderer.DrawMesh(oneQuad, Identity, material);    // far (z 0.5)
        renderer.DrawMesh(threeQuads, Identity, material); // near (z -0.5)
    });
    EXPECT_EQ(counters.drawCalls, 2u);
    EXPECT_EQ(counters.triangles, 8u);
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Size) * Size * 4);
    renderer.ReadFramebuffer(pixels.data(), Size, Size); // the frame finishes either way
}
