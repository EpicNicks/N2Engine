#include <gtest/gtest.h>

#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"
#include "common/PostProcessScenes.hpp"

// Post-processing and render textures (issue #445) on Vulkan (Mesa lavapipe in CI, validation on: the fixture fails a test that
// leaves a validation error or warning). The frame's pass is ended and resumed in the middle of the frame, so these also prove
// the layouts and barriers; the frames are compared with Common::ApplyPostProcessCpu, within the 8-bit rounding of the
// half-size bloom images. The software renderer runs the same scenes (SoftwarePostProcessTests.cpp).

using namespace VulkanTesting;
using namespace VulkanTesting::Draw;

namespace
{
    class VulkanPostProcessTest : public VulkanDrawTest
    {
    };
}

TEST_F(VulkanPostProcessTest, SupportsPostProcessWithItsShaders)
{
    EXPECT_TRUE(R().SupportsPostProcess());
}

TEST_F(VulkanPostProcessTest, BloomMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckBloom(12);
}

TEST_F(VulkanPostProcessTest, InvertVignetteAndChromaticAberrationMatchTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckComposite(4);
}

TEST_F(VulkanPostProcessTest, BlurMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckBlur(12);
}

TEST_F(VulkanPostProcessTest, WarpMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckWarp(12);
}

TEST_F(VulkanPostProcessTest, DrawsAfterThePostProcessAreNotProcessed)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckLaterDrawsAreUntouched();
}

TEST_F(VulkanPostProcessTest, CapturesAreUprightOpaqueAndSeenByLaterDraws)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckCapture(2);
}

TEST_F(VulkanPostProcessTest, APostProcessAfterAResizeUsesNewImages)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckLaterDrawsAreUntouched();
    ASSERT_TRUE(R().SetRenderTargetSize(32, 32));
    Renderer::Common::PostProcessSettings settings;
    settings.bloom = true;
    settings.invert = 0.5f;
    R().BeginFrame();
    R().ApplyPostProcess(settings);
    R().EndFrame();
    ASSERT_TRUE(R().SetRenderTargetSize(PostProcessScenes::Size, PostProcessScenes::Size));
    scenes.CheckLaterDrawsAreUntouched();
}
