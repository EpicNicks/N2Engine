#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <math/Vector3.hpp>
#include <renderer/common/GpuLighting.hpp>
#include <renderer/common/GpuShaderInterface.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/SceneLighting.hpp>
#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"

// The WebGPU backend's lit drawing at renderer level (#155, stage G3b): what is particular to this backend and has no Vulkan
// test to mirror (the ported Vulkan tests are WebGpuRendererLightingTests.cpp, WebGpuRendererNormalMapPbrTests.cpp and
// WebGpuRendererBlendTests.cpp). The lighting block's CPU copy against BuildLightingBlock, the copies of the block in the
// uniform ring (one per chunk per version, so a tiny ring that chains many chunks must still give every lit draw the lighting
// of its time, and an unlit draw must never disturb them), mid-frame SetViewProjection with the lit shader, and the lit
// pipelines in the cache. Every draw here is an exact pixel check: ambient light only, so the lit colour is albedo x ambient.

using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;
using N2Engine::Math::Vector3;
using Renderer::Common::IndexRange;
using Renderer::Common::SceneLightingData;
namespace ShaderInterface = Renderer::Gpu::ShaderInterface;

namespace
{
    SceneLightingData Ambient(const float grey)
    {
        SceneLightingData lighting;
        lighting.ambientColor = Vector3(grey, grey, grey);
        return lighting;
    }

    RenderState NoDepth()
    {
        RenderState state;
        state.depthTest = false;
        state.depthWrite = false;
        return state;
    }

    class WebGpuLitDrawTest : public WebGpuDrawTest
    {
    protected:
        void SetUp() override
        {
            WebGpuDrawTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            _lit = R().GetStandardLitShader();
            ASSERT_NE(_lit, nullptr);
        }

        /// A lit material with the OpenGL defaults (white, smoothness 0.5): with ambient light only the colour is albedo x ambient
        IMaterial* LitMaterial()
        {
            IMaterial* material = R().CreateMaterial(_lit);
            EXPECT_NE(material, nullptr);
            return material;
        }

        Renderer::Common::IShader* _lit = nullptr;
    };
}

TEST_F(WebGpuLitDrawTest, TheLightingBlockIsWhatBuildLightingBlockMakesAndStartsAsTheEmptyScene)
{
    const std::vector<std::uint8_t> initial = R().GetLightingBlockForTest();
    const Renderer::Gpu::LightingBlock empty = Renderer::Gpu::BuildLightingBlock(SceneLightingData{}, Vector3(0.0f, 0.0f, 0.0f));
    ASSERT_EQ(initial.size(), static_cast<std::size_t>(ShaderInterface::kLightingBlockSize));
    EXPECT_TRUE(std::equal(initial.begin(), initial.end(), empty.begin())) << "before the first update: the empty scene seen from the origin";

    SceneLightingData lighting = Ambient(0.3f);
    Renderer::Common::DirectionalLightData sun;
    sun.direction = Vector3(0.0f, 0.0f, -1.0f);
    sun.intensity = 0.5f;
    lighting.directionalLights.push_back(sun);
    R().UpdateSceneLighting(lighting, Vector3(1.0f, 2.0f, 3.0f));
    const Renderer::Gpu::LightingBlock built = Renderer::Gpu::BuildLightingBlock(lighting, Vector3(1.0f, 2.0f, 3.0f));
    const std::vector<std::uint8_t> kept = R().GetLightingBlockForTest();
    ASSERT_EQ(kept.size(), built.size());
    EXPECT_TRUE(std::equal(kept.begin(), kept.end(), built.begin())) << "the kept copy is the built block, byte for byte";
    EXPECT_FALSE(std::equal(kept.begin(), kept.end(), empty.begin())) << "and it changed";
}

TEST_F(WebGpuLitDrawTest, ATinyRingChainsManyChunksAndEveryLitDrawStillSeesTheLightingOfItsTime)
{
    // Chunks of about 1 KiB: each holds one or two draws, so the copy of the lighting block lands in a different chunk again
    // and again, between lit draws, unlit draws and updates. The picture is the last of 40 rounds (no depth: the last draw of a
    // pixel wins), and the second frame, which reuses the chunks, must be the same picture
    R().SetRingChunkBytesForTest(1024);
    IMesh* halves = MakeMesh(TwoHalves());
    IMesh* pixel = MakeMesh(Quad(-1.0f, -1.0f, -1.0f + 2.0f / Size, -1.0f + 2.0f / Size, 0.0f));
    ASSERT_NE(halves, nullptr);
    ASSERT_NE(pixel, nullptr);
    IMaterial* lit = LitMaterial();
    IMaterial* green = Colour(0.0f, 1.0f, 0.0f);
    const IndexRange left{0, 6};
    const IndexRange right{6, 6};
    const RenderState state = NoDepth();

    Frame frames[2];
    for (Frame& frame : frames)
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        for (int round = 0; round < 40; ++round)
        {
            R().UpdateSceneLighting(Ambient(0.2f), Vector3(0.0f, 0.0f, 100.0f));
            R().DrawMesh(halves, Identity, lit, state, left);
            R().DrawMesh(pixel, Identity, green, state); // an unlit draw between the lit ones: names a copy it never reads
            R().UpdateSceneLighting(Ambient(0.8f), Vector3(0.0f, 0.0f, 100.0f));
            R().DrawMesh(halves, Identity, lit, state, right);
        }
        R().EndFrame();
        frame = ReadFrame();
        EXPECT_NEAR(frame.At(16, 32).r, 51, 1) << "the left half was drawn under the ambient 0.2";
        EXPECT_NEAR(frame.At(48, 32).r, 204, 1) << "the right half under 0.8";
        EXPECT_EQ(frame.At(0, 0), kGreen) << "the unlit pixel is its own colour, whatever the lighting around it";
    }
    EXPECT_EQ(frames[0], frames[1]) << "the second frame reuses every chunk and draws the same picture";
    EXPECT_GE(R().GetRingChunkCountForTest(), 8u) << "the ring chained many small chunks";
}

TEST_F(WebGpuLitDrawTest, AnUnlitDrawInTheSmallestPossibleChunkNamesALightingRangeThatIsInBounds)
{
    // A chunk is never smaller than the lighting block (752 bytes, 768 aligned): group 0's binding 1 is bound at offset 0 by a
    // draw that does not read it, and WebGPU validates that range against the buffer (the fixture fails the test on an error)
    R().SetRingChunkBytesForTest(1);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    ASSERT_NE(quad, nullptr);
    const Frame frame = RenderFrameOf([&] { R().DrawMesh(quad, Identity, red); });
    EXPECT_EQ(frame.Count(kRed), Size * Size);
    EXPECT_GE(R().GetRingChunkCountForTest(), 1u);
}

TEST_F(WebGpuLitDrawTest, EveryLitDrawOfAFrameKeepsTheViewItWasMadeWith)
{
    // 16 x 16 cells of 4 x 4 pixels. A draw per cell, only the cells of a checkerboard, each with its own view (a translation
    // moving a cell-sized quad from cell (0, 0) to its cell), set just before it. A draw that took another draw's matrices (a
    // stale ring block) would light the wrong cell or none. Ambient 0.5 on white: every drawn pixel is 128 (127 or 128)
    constexpr int kCells = 16;
    constexpr int kCellPixels = Size / kCells;
    const float cell = 2.0f / static_cast<float>(kCells);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, -1.0f + cell, -1.0f + cell, 0.0f));
    IMaterial* lit = LitMaterial();
    ASSERT_NE(quad, nullptr);
    ASSERT_NE(lit, nullptr);

    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().UpdateSceneLighting(Ambient(0.5f), Vector3(0.0f, 0.0f, 100.0f));
    for (int i = 0; i < kCells * kCells; ++i)
    {
        const int cx = i % kCells;
        const int cy = i / kCells;
        if ((cx + cy) % 2 != 0)
        {
            continue;
        }
        float view[16];
        Translation(view, cell * static_cast<float>(cx), cell * static_cast<float>(cy), 0.0f);
        R().SetViewProjection(view, Identity);
        R().DrawMesh(quad, Identity, lit);
    }
    R().EndFrame();
    const Frame frame = ReadFrame();

    int wrong = 0;
    for (int cy = 0; cy < kCells; ++cy)
    {
        for (int cx = 0; cx < kCells; ++cx)
        {
            const bool drawn = (cx + cy) % 2 == 0;
            for (int py = 0; py < kCellPixels; ++py)
            {
                for (int px = 0; px < kCellPixels; ++px)
                {
                    const Rgb pixel = frame.At(cx * kCellPixels + px, cy * kCellPixels + py);
                    const bool litPixel = pixel.r >= 127 && pixel.r <= 128 && pixel.g == pixel.r && pixel.b == pixel.r;
                    const bool blackPixel = pixel == kBlack;
                    wrong += (drawn ? litPixel : blackPixel) ? 0 : 1;
                }
            }
        }
    }
    EXPECT_EQ(wrong, 0) << "pixels that are not their cell's colour";
}

TEST_F(WebGpuLitDrawTest, TheLitShaderHasAPipelinePerStateAndCachesThem)
{
    // opaque, blended, additive: three pipelines (additive without blend is the opaque one), each made once
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);
    RenderState opaque = RenderState::Opaque();
    RenderState blended = RenderState::Transparent();
    RenderState additive = RenderState::Transparent();
    additive.additive = true;
    RenderState additiveOnly = RenderState::Opaque();
    additiveOnly.additive = true;
    for (int round = 0; round < 2; ++round)
    {
        for (const RenderState& state : {opaque, blended, additive, additiveOnly})
        {
            EXPECT_TRUE(R().CreatePipelineForTest(ShaderInterface::BuiltInShader::Lit, state));
        }
        EXPECT_EQ(R().GetPipelineCountForTest(), 3u) << "round " << round;
        EXPECT_EQ(R().GetPipelineCreateAttemptCountForTest(), 3u);
    }
    EXPECT_TRUE(R().CreatePipelineForTest(ShaderInterface::BuiltInShader::Unlit, opaque));
    EXPECT_TRUE(R().CreatePipelineForTest(ShaderInterface::BuiltInShader::Text, blended));
    EXPECT_EQ(R().GetPipelineCountForTest(), 5u) << "the same state in another shader is another pipeline";
}
