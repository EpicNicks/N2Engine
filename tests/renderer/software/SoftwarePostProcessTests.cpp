#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include <renderer/common/PostProcess.hpp>
#include <renderer/software/SoftwareRenderer.hpp>

#include "common/PostProcessScenes.hpp"

// Post-processing and render textures (issue #445) on the software renderer: ApplyPostProcess runs ApplyPostProcessCpu on the
// colour buffer at its place among the frame's draws, CaptureFrame copies the colour buffer into a render texture. Headless, so
// CI runs it everywhere. The same scenes run on Vulkan (VulkanPostProcessTests.cpp) and WebGPU (WebGpuRendererPostProcessTests.cpp).

using namespace Renderer::Common;
using Renderer::Software::SoftwareRenderer;

namespace
{
    class SoftwarePostProcessTest : public ::testing::Test
    {
    protected:
        SoftwareRenderer renderer;

        void SetUp() override { ASSERT_TRUE(renderer.Initialize(nullptr, PostProcessScenes::Size, PostProcessScenes::Size)); }
        void TearDown() override { renderer.Shutdown(); }
    };
}

TEST(PostProcessMaths, SanitizedClampsEveryFieldAndNonFiniteTakesTheDefault)
{
    PostProcessSettings settings;
    settings.bloom = true;
    settings.bloomThreshold = 3.0f;
    settings.bloomIntensity = -1.0f;
    settings.bloomIterations = 99;
    settings.bloomTint[0] = std::numeric_limits<float>::quiet_NaN();
    settings.invert = 2.0f;
    settings.chromaticAberration = 1.0f;
    settings.vignette = -0.5f;
    const PostProcessSettings clean = PostProcessSettings::Sanitized(settings);
    EXPECT_TRUE(clean.bloom);
    EXPECT_FLOAT_EQ(clean.bloomThreshold, 1.0f);
    EXPECT_FLOAT_EQ(clean.bloomIntensity, 0.0f);
    EXPECT_EQ(clean.bloomIterations, PostProcessSettings::MaxBloomIterations);
    EXPECT_FLOAT_EQ(clean.bloomTint[0], 1.0f);
    EXPECT_FLOAT_EQ(clean.invert, 1.0f);
    EXPECT_FLOAT_EQ(clean.chromaticAberration, PostProcessSettings::MaxChromaticAberration);
    EXPECT_FLOAT_EQ(clean.vignette, 0.0f);
    EXPECT_FALSE(clean.BloomActive()) << "an intensity of 0 adds nothing";
    EXPECT_TRUE(clean.IsActive()) << "invert is on";
    EXPECT_FALSE(PostProcessSettings{}.IsActive()) << "the defaults change nothing";
}

TEST(PostProcessMaths, TheBlurWeightsSumToOne)
{
    float sum = BloomBlurWeights[0];
    for (int k = 1; k < 5; ++k)
    {
        sum += 2.0f * BloomBlurWeights[k];
    }
    EXPECT_NEAR(sum, 1.0f, 1e-6f);
}

TEST(PostProcessMaths, TheCpuPassLeavesAFrameAloneWhenInactiveAndInvertsItFully)
{
    std::vector<std::uint32_t> pixels(16, 0xFF204080u); // r 0x80, g 0x40, b 0x20, a 0xFF
    const std::vector<std::uint32_t> before = pixels;
    ApplyPostProcessCpu(pixels.data(), 4, 4, PostProcessSettings{});
    EXPECT_EQ(pixels, before);
    PostProcessSettings invert;
    invert.invert = 1.0f;
    ApplyPostProcessCpu(pixels.data(), 4, 4, invert);
    for (const std::uint32_t pixel : pixels)
    {
        EXPECT_EQ(pixel, 0xFFDFBF7Fu) << std::hex << pixel; // 255 - each channel, alpha kept
    }
    ApplyPostProcessCpu(nullptr, 4, 4, invert); // ignored
}

TEST(PostProcessMaths, ParamsCarryTheSettings)
{
    PostProcessSettings settings;
    settings.bloom = true;
    settings.bloomThreshold = 0.25f;
    settings.bloomIntensity = 3.0f;
    settings.invert = 0.5f;
    settings.vignette = 0.75f;
    settings.bloomTint[2] = 0.5f;
    const PostProcessParams params = PostProcessParams::For(PostPassMode::Composite, settings);
    EXPECT_FLOAT_EQ(params.a[0], 3.0f);
    EXPECT_FLOAT_EQ(params.a[1], 0.25f);
    EXPECT_FLOAT_EQ(params.a[2], 3.0f);
    EXPECT_FLOAT_EQ(params.a[3], 0.5f);
    EXPECT_FLOAT_EQ(params.b[3], 0.75f);
    EXPECT_FLOAT_EQ(params.c[2], 0.5f);
    EXPECT_FLOAT_EQ(params.c[3], 0.0f);
    settings.bloom = false;
    EXPECT_FLOAT_EQ(PostProcessParams::For(PostPassMode::Composite, settings).a[2], 0.0f) << "no bloom adds nothing";
    EXPECT_EQ(BloomExtent(64), 32u);
    EXPECT_EQ(BloomExtent(1), 1u);
}

TEST_F(SoftwarePostProcessTest, SupportsPostProcess)
{
    EXPECT_TRUE(renderer.SupportsPostProcess());
}

TEST_F(SoftwarePostProcessTest, BloomMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(renderer);
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckBloom(1);
}

TEST_F(SoftwarePostProcessTest, InvertVignetteAndChromaticAberrationMatchTheReference)
{
    PostProcessScenes::Scenes scenes(renderer);
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckComposite(1);
}

TEST_F(SoftwarePostProcessTest, DrawsAfterThePostProcessAreNotProcessed)
{
    PostProcessScenes::Scenes scenes(renderer);
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckLaterDrawsAreUntouched();
}

TEST_F(SoftwarePostProcessTest, CapturesAreUprightOpaqueAndSeenByLaterDraws)
{
    PostProcessScenes::Scenes scenes(renderer);
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckCapture(1);
}
