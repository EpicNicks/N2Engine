#include <gtest/gtest.h>

#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "WebGpuDrawTestUtil.hpp"
#include "WebGpuTestFixture.hpp"
#include "common/PostProcessScenes.hpp"

// Post-processing and render textures (issue #445) on WebGPU (Dawn over Mesa lavapipe in CI; the fixture fails a test that leaves
// a device error): the frame's render pass ends and a loading one resumes it, the passes draw post.wgsl. The frames are compared
// with Common::ApplyPostProcessCpu, within the 8-bit rounding of the half-size bloom textures. The software renderer runs the
// same scenes (SoftwarePostProcessTests.cpp), and so does Vulkan (VulkanPostProcessTests.cpp).

using namespace WebGpuTesting;
using namespace WebGpuTesting::Draw;

namespace
{
    class WebGpuPostProcessTest : public WebGpuDrawTest
    {
    };
}

TEST_F(WebGpuPostProcessTest, SupportsPostProcess)
{
    EXPECT_TRUE(R().SupportsPostProcess());
}

TEST_F(WebGpuPostProcessTest, BloomMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckBloom(12);
}

TEST_F(WebGpuPostProcessTest, InvertVignetteAndChromaticAberrationMatchTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckComposite(4);
}

TEST_F(WebGpuPostProcessTest, BlurMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckBlur(12);
}

TEST_F(WebGpuPostProcessTest, WarpMatchesTheReference)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckWarp(12);
}

TEST_F(WebGpuPostProcessTest, DrawsAfterThePostProcessAreNotProcessed)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckLaterDrawsAreUntouched();
}

TEST_F(WebGpuPostProcessTest, CapturesAreUprightOpaqueAndSeenByLaterDraws)
{
    PostProcessScenes::Scenes scenes(R());
    ASSERT_TRUE(scenes.Ready());
    scenes.CheckCapture(2);
}
