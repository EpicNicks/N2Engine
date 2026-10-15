#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/common/RenderTypes.hpp>
#include <renderer/common/TextureOptions.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>
#include <renderer/vulkan/VulkanShaderInterface.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// The Vulkan backend's shaders, pipeline layout and pipeline cache (#43, stage B), on a Vulkan device (Mesa lavapipe in CI)
// with validation on (zero errors and warnings, nothing leaked at Shutdown):
//   - the SPIR-V of all three built-in shaders makes shader modules, and a pipeline against the one shared layout, for
//     every depth/write/cull state and for wireframe (where the device has fillModeNonSolid), with no validation message;
//   - the cache returns the same pipeline for an equal key, its size is bounded by the distinct (shader, RenderState,
//     wireframe) keys, and it is not invalidated by resizing the target (the viewport and scissor are dynamic);
//   - the per-draw uniform ring grows: ten thousand draws in one frame all land, the ring and the descriptor pools are
//     reused as the frame slots come round, and nothing a frame in flight reads is rewritten (validation says so);
//   - Shutdown destroys every pipeline, layout, pool, module, ring and default texture before the allocator and device
//     (the fixture's leak check), and a renderer initialized again starts clean.
// The tests that draw need the built-in shaders (N2_REQUIRE_BUILT_IN_SHADERS): see VulkanTestFixture.hpp.

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;
using Renderer::Common::CullMode;
using Renderer::Common::IndexRange;
using Renderer::Common::TextureOptions;
using Renderer::Vulkan::VulkanRenderer;
using Renderer::Vulkan::ShaderInterface::BuiltInShader;

namespace
{
    constexpr BuiltInShader AllShaders[] = {BuiltInShader::Unlit, BuiltInShader::Lit, BuiltInShader::Text};
    constexpr CullMode AllCulls[] = {CullMode::None, CullMode::Back, CullMode::Front};

    RenderState StateOf(const bool depthTest, const bool depthWrite, const CullMode cull)
    {
        RenderState state;
        state.depthTest = depthTest;
        state.depthWrite = depthWrite;
        state.cull = cull;
        return state;
    }

    /// A row-major matrix that scales x and y by `scale` and moves the origin to (x, y)
    void ScaleAndTranslate(float (&m)[16], const float scale, const float x, const float y)
    {
        const float values[16] = {scale, 0, 0, x, 0, scale, 0, y, 0, 0, 1, 0, 0, 0, 0, 1};
        std::copy(std::begin(values), std::end(values), m);
    }
}

TEST_F(VulkanTest, TheBuiltInShadersAreCompiledIntoThisBuild)
{
    // Skips where the build has no compiler (CMake warned), fails under N2_REQUIRE_VULKAN=1: the Linux CI legs can never
    // silently skip the draw tests
    N2_REQUIRE_BUILT_IN_SHADERS();
    EXPECT_TRUE(VulkanRenderer::HasBuiltInShaders());
    EXPECT_NE(R().GetStandardUnlitShader(), nullptr);
}

TEST_F(VulkanTest, WithoutCompiledShadersTheRendererStillClearsAndReadsBackAndDrawsNothing)
{
    if (VulkanRenderer::HasBuiltInShaders())
    {
        GTEST_SKIP() << "this build holds the compiled shaders: the no-shader fallback is not in use";
    }
    // The fallback of a build with neither glslangValidator nor glslc: everything but the shaders works
    EXPECT_EQ(R().GetStandardUnlitShader(), nullptr);
    EXPECT_EQ(R().GetStandardLitShader(), nullptr);
    EXPECT_EQ(R().CreateMaterial(nullptr), nullptr);
    EXPECT_FALSE(R().CreatePipelineForTest(BuiltInShader::Unlit, RenderState{}, false));
    Renderer::Common::MeshData data = Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f);
    Renderer::Common::IMesh* mesh = R().CreateMesh(data);
    ASSERT_NE(mesh, nullptr);
    R().BeginFrame();
    R().DrawMesh(mesh, Identity, nullptr);
    ClearTo(R(), {9, 8, 7, 255});
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {9, 8, 7, 255}));
}

TEST_F(VulkanDrawTest, AllThreeBuiltInShadersMakePipelinesForEveryDepthWriteCullAndWireframeCombination)
{
    const bool wireframe = R().IsWireframeSupportedForTest();
    std::uint32_t expected = 0;
    for (const bool wire : {false, true})
    {
        if (wire && !wireframe)
        {
            continue;
        }
        for (const BuiltInShader shader : AllShaders)
        {
            for (const bool depthTest : {false, true})
            {
                for (const bool depthWrite : {false, true})
                {
                    for (const CullMode cull : AllCulls)
                    {
                        ASSERT_TRUE(R().CreatePipelineForTest(shader, StateOf(depthTest, depthWrite, cull), wire))
                            << "shader " << static_cast<int>(shader) << " depthTest " << depthTest << " depthWrite " << depthWrite
                            << " cull " << static_cast<int>(cull) << " wireframe " << wire;
                        ++expected;
                    }
                }
            }
        }
    }
    EXPECT_EQ(R().GetPipelineCountForTest(), expected) << "one pipeline per distinct key";

    // The same keys again find the pipelines made: the cache does not grow
    for (const BuiltInShader shader : AllShaders)
    {
        for (const CullMode cull : AllCulls)
        {
            ASSERT_TRUE(R().CreatePipelineForTest(shader, StateOf(true, true, cull), false));
        }
    }
    EXPECT_EQ(R().GetPipelineCountForTest(), expected);

    // Wireframe is refused, not faked, where the device cannot draw it
    if (!wireframe)
    {
        EXPECT_FALSE(R().CreatePipelineForTest(BuiltInShader::Unlit, RenderState{}, true));
    }
}

TEST_F(VulkanDrawTest, BlendAndAdditiveAreAPartOfThePipelineKey)
{
    RenderState additive = RenderState::Transparent();
    additive.additive = true;
    ASSERT_TRUE(R().CreatePipelineForTest(BuiltInShader::Unlit, RenderState::Opaque(), false));
    EXPECT_EQ(R().GetPipelineCountForTest(), 1u);
    ASSERT_TRUE(R().CreatePipelineForTest(BuiltInShader::Unlit, RenderState::Transparent(), false));
    EXPECT_EQ(R().GetPipelineCountForTest(), 2u);
    ASSERT_TRUE(R().CreatePipelineForTest(BuiltInShader::Unlit, additive, false));
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u);
    ASSERT_TRUE(R().CreatePipelineForTest(BuiltInShader::Unlit, additive, false));
    EXPECT_EQ(R().GetPipelineCountForTest(), 3u) << "an equal key is the same pipeline";
}

TEST_F(VulkanDrawTest, ThousandsOfDrawsMakeOnlyThePipelinesTheirStatesNeed)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(quad, nullptr);
    RenderState noCull;
    noCull.cull = CullMode::None;
    RenderState noWrite;
    noWrite.depthWrite = false;
    RenderState noTest;
    noTest.depthTest = false;

    const Frame frame = RenderFrame([&]
    {
        for (int i = 0; i < 500; ++i)
        {
            R().DrawMesh(quad, Identity, red);
            R().DrawMesh(quad, Identity, red, noCull);
            R().DrawMesh(quad, Identity, red, noWrite);
            R().DrawMesh(quad, Identity, red, noTest);
        }
    });
    EXPECT_EQ(R().GetPipelineCountForTest(), 4u) << "four distinct states, 2000 draws";
    EXPECT_EQ(frame.Count(kRed), 32 * 32);
}

TEST_F(VulkanDrawTest, PipelinesSurviveAnyNumberOfResizesAndStillDrawCorrectly)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f)); // half the frame in each direction
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), 32 * 32);
    const std::uint32_t pipelines = R().GetPipelineCountForTest();
    ASSERT_GE(pipelines, 1u);

    struct Target
    {
        std::uint32_t width, height;
    };
    for (const Target target : {Target{32, 48}, Target{16, 16}, Target{128, 64}, Target{64, 64}, Target{8, 8}, Target{96, 40}})
    {
        ASSERT_TRUE(R().SetRenderTargetSize(target.width, target.height));
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        R().DrawMesh(quad, Identity, red);
        R().EndFrame();
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(target.width) * target.height * 4);
        R().ReadFramebuffer(pixels.data(), static_cast<int>(target.width), static_cast<int>(target.height));
        std::uint32_t count = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            count += (pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 0) ? 1u : 0u;
        }
        EXPECT_EQ(count, (target.width / 2) * (target.height / 2)) << target.width << "x" << target.height;
        EXPECT_EQ(R().GetPipelineCountForTest(), pipelines) << "a new size needs no new pipeline (dynamic viewport)";
    }
}

TEST_F(VulkanDrawTest, TenThousandDrawsInOneFrameAllLandAndTheRingGrowsAndIsReused)
{
    constexpr int Side = 100;
    ASSERT_TRUE(R().SetRenderTargetSize(Side, Side));
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-1.0f, -1.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(quad, nullptr);
    const std::uint32_t chunksBefore = R().GetRingChunkCountForTest();

    // One draw per pixel: the quad shrunk to one pixel (0.02 NDC at 100 pixels) and moved to the pixel's centre
    const auto drawAllPixels = [&]
    {
        for (int y = 0; y < Side; ++y)
        {
            for (int x = 0; x < Side; ++x)
            {
                float model[16];
                const float centreX = (static_cast<float>(x) + 0.5f) / (Side / 2) - 1.0f;
                const float centreY = (static_cast<float>(y) + 0.5f) / (Side / 2) - 1.0f;
                ScaleAndTranslate(model, 1.0f / Side, centreX, centreY);
                R().DrawMesh(quad, model, red);
            }
        }
    };
    const auto countRed = [&]
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(Side) * Side * 4);
        R().ReadFramebuffer(pixels.data(), Side, Side);
        int count = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            count += (pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 0) ? 1 : 0;
        }
        return count;
    };

    for (int frameNumber = 0; frameNumber < 3; ++frameNumber)
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        drawAllPixels();
        R().EndFrame();
        EXPECT_EQ(countRed(), Side * Side) << "frame " << frameNumber << ": every one of the 10000 draws landed";
    }
    EXPECT_GE(R().GetRingChunkCountForTest(), chunksBefore + 3) << "the ring chained bigger chunks to hold the draws";
    const std::uint32_t chunks = R().GetRingChunkCountForTest();

    // The chunks are kept and reused by the frames after: a frame of the same size makes no new one
    for (int frameNumber = 0; frameNumber < 3; ++frameNumber)
    {
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        drawAllPixels();
        R().EndFrame();
    }
    EXPECT_EQ(countRed(), Side * Side);
    EXPECT_EQ(R().GetRingChunkCountForTest(), chunks);
}

TEST_F(VulkanDrawTest, ManyMoreFramesThanSlotsInFlightEachDrawWithTheirOwnValuesOneMaterialSetBetweenDraws)
{
    IMaterial* material = R().CreateMaterial(_unlit);
    ASSERT_NE(material, nullptr);
    IMesh* thirds[3] = {MakeMesh(Quad(-1.0f, -1.0f, -1.0f / 3, 1.0f, 0.0f)), MakeMesh(Quad(-1.0f / 3, -1.0f, 1.0f / 3, 1.0f, 0.0f)),
                        MakeMesh(Quad(1.0f / 3, -1.0f, 1.0f, 1.0f, 0.0f))};
    ASSERT_NE(thirds[2], nullptr);
    const std::uint8_t texel[4] = {255, 255, 255, 255};
    TextureOptions nearest;
    nearest.filter = Renderer::Common::TextureFilter::Nearest;
    nearest.mipmaps = false;
    ITexture* white = R().CreateTexture(texel, 1, 1, 4, nearest);
    ASSERT_NE(white, nullptr);

    for (int frameNumber = 0; frameNumber < 24; ++frameNumber)
    {
        // Three colours from one material, one per draw, each different in every frame
        const int base = 5 + frameNumber * 9;
        const Rgb colours[3] = {Rgb{static_cast<std::uint8_t>(base), 0, 0}, Rgb{0, static_cast<std::uint8_t>(base + 100), 0},
                                Rgb{0, 0, static_cast<std::uint8_t>(base + 200 > 255 ? 255 - frameNumber : base + 200)}};
        R().BeginFrame();
        R().SetViewProjection(Identity, Identity);
        // The textured draw in the middle makes a second material set (another sampler binding) in the same frame
        for (int i = 0; i < 3; ++i)
        {
            material->SetTexture(i == 1 ? white : nullptr);
            material->SetInt("uHasTexture", i == 1 ? 1 : 0);
            material->SetColor("uAlbedo", Unorm(colours[i].r), Unorm(colours[i].g), Unorm(colours[i].b), 1.0f);
            R().DrawMesh(thirds[i], Identity, material);
        }
        R().EndFrame();
        const Frame frame = ReadFrame();
        EXPECT_TRUE(SameColour(frame.At(Size / 6, 30), colours[0])) << "frame " << frameNumber << " left";
        EXPECT_TRUE(SameColour(frame.At(Size / 2, 30), colours[1])) << "frame " << frameNumber << " middle";
        EXPECT_TRUE(SameColour(frame.At(Size - Size / 6, 30), colours[2])) << "frame " << frameNumber << " right";
    }
}

TEST_F(VulkanDrawTest, ARendererInitializedAgainStartsCleanAndDrawsAgain)
{
    IMaterial* red = Colour(1.0f, 0.0f, 0.0f);
    IMesh* quad = MakeMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(RenderFrame([&] { R().DrawMesh(quad, Identity, red); }).Count(kRed), 32 * 32);
    EXPECT_GE(R().GetPipelineCountForTest(), 1u);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 1u);

    R().Shutdown();
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);
    EXPECT_EQ(R().GetLiveMaterialCountForTest(), 0u);
    EXPECT_EQ(R().GetStandardUnlitShader(), nullptr) << "no shaders without a device";

    // The old material and mesh are gone with the old device; a new one is a fresh renderer
    ASSERT_TRUE(R().Initialize(nullptr, Size, Size));
    IMaterial* green = R().CreateMaterial(R().GetStandardUnlitShader());
    ASSERT_NE(green, nullptr);
    green->SetColor("uAlbedo", 0.0f, 1.0f, 0.0f, 1.0f);
    IMesh* again = R().CreateMesh(Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(R().GetPipelineCountForTest(), 0u);
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().DrawMesh(again, Identity, green);
    R().EndFrame();
    EXPECT_EQ(ReadFrame().Count(kGreen), 32 * 32);
    EXPECT_EQ(R().GetPipelineCountForTest(), 1u);
}

TEST_F(VulkanDrawTest, LitAndTextPipelinesAreMadeButTheirShadersAreNotHandedOut)
{
    // Stage B compiles and pipeline-checks all three shaders; only unlit draws. Lit is turned on in stage C, text in E
    EXPECT_EQ(R().GetStandardLitShader(), nullptr);
    EXPECT_EQ(R().GetStandardTextShader(), nullptr);
    for (const BuiltInShader shader : {BuiltInShader::Lit, BuiltInShader::Text})
    {
        Renderer::Common::IShader* builtIn = R().GetBuiltInShaderForTest(shader);
        ASSERT_NE(builtIn, nullptr);
        EXPECT_TRUE(R().IsValidShader(builtIn));
        EXPECT_EQ(R().CreateMaterial(builtIn), nullptr) << "no material of it yet";
        EXPECT_TRUE(R().CreatePipelineForTest(shader, RenderState{}, false));
    }
}
