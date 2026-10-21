#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "WebGpuTestFixture.hpp"

// The WebGPU backend's core (#155, stage G2), run headless on Dawn's Vulkan backend (Mesa lavapipe in CI): Initialize(nullptr,
// w, h), a frame that clears the offscreen target, ReadFramebuffer, SetRenderTargetSize. Written to mirror the Vulkan backend's
// stage 0 tests (tests/renderer/vulkan/VulkanRendererTests.cpp). Errors, device loss, shutdown and leaks are in
// WebGpuRendererLifecycleTests.cpp.
//
// Every test runs under the fixture's rules: the device reported no error and no WebGPU object outlived the renderers. Where
// there is no adapter the tests skip, unless N2_REQUIRE_WEBGPU=1 (see WebGpuTestFixture.hpp).

using Renderer::WebGpu::InitStatus;
using Renderer::WebGpu::WebGpuRenderer;
using namespace WebGpuTesting; // the fixture and helpers

namespace
{
    struct Size
    {
        int w, h;
    };

    class WebGpuSizeTest : public WebGpuTest, public ::testing::WithParamInterface<Size>
    {
    };

    /// The pixel the orientation tests put at (x, y) of the target, y counted from the top row (texture row 0)
    Rgba PatternPixel(int x, int y)
    {
        return {(x * 7 + y * 13) % 256, (x * 3 + y * 29 + 5) % 256, (x * 11 + y * 5 + 100) % 256, 255 - (x + y) % 64};
    }

    /// Writes PatternPixel into the whole colour target through the raw queue (the target's usage includes CopyDst)
    void WritePattern(const WebGpuRenderer& renderer, int width, int height)
    {
        std::vector<std::uint8_t> data(static_cast<std::size_t>(width) * height * 4);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const Rgba p = PatternPixel(x, y);
                std::uint8_t* out = &data[(static_cast<std::size_t>(y) * width + x) * 4];
                out[0] = static_cast<std::uint8_t>(p.r);
                out[1] = static_cast<std::uint8_t>(p.g);
                out[2] = static_cast<std::uint8_t>(p.b);
                out[3] = static_cast<std::uint8_t>(p.a);
            }
        }
        WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        destination.texture = renderer.GetColorTextureForTest();
        destination.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.offset = 0;
        layout.bytesPerRow = static_cast<std::uint32_t>(width) * 4;
        layout.rowsPerImage = static_cast<std::uint32_t>(height);
        const WGPUExtent3D extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        wgpuQueueWriteTexture(renderer.GetQueueForTest(), &destination, data.data(), data.size(), &layout, &extent);
    }
}

TEST_F(WebGpuTest, InitializesHeadlessWithAnOkStatusAnAdapterAndTheTargetFormats)
{
    EXPECT_EQ(R().GetInitStatus(), InitStatus::Ok);
    EXPECT_TRUE(R().GetInitMessage().empty());
    EXPECT_FALSE(R().GetAdapterName().empty());
    EXPECT_EQ(R().GetBackendType(), WGPUBackendType_Vulkan);
    EXPECT_EQ(R().GetRenderWidth(), 8u);
    EXPECT_EQ(R().GetRenderHeight(), 8u);
    EXPECT_STREQ(R().GetRendererName(), "WebGPU Renderer");
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
    EXPECT_FALSE(R().IsDeviceLost());

    ASSERT_NE(R().GetInstanceForTest(), nullptr);
    ASSERT_NE(R().GetDeviceForTest(), nullptr);
    ASSERT_NE(R().GetQueueForTest(), nullptr);

    const WGPUTexture color = R().GetColorTextureForTest();
    const WGPUTexture depth = R().GetDepthTextureForTest();
    ASSERT_NE(color, nullptr);
    ASSERT_NE(depth, nullptr);
    EXPECT_EQ(wgpuTextureGetFormat(color), WGPUTextureFormat_RGBA8Unorm);
    EXPECT_EQ(wgpuTextureGetFormat(depth), WGPUTextureFormat_Depth32Float);
    EXPECT_EQ(wgpuTextureGetFormat(color), WebGpuRenderer::GetColorFormat());
    EXPECT_EQ(wgpuTextureGetFormat(depth), WebGpuRenderer::GetDepthFormat());
    EXPECT_EQ(wgpuTextureGetWidth(color), 8u);
    EXPECT_EQ(wgpuTextureGetHeight(color), 8u);
    EXPECT_EQ(wgpuTextureGetWidth(depth), 8u);
    EXPECT_EQ(wgpuTextureGetHeight(depth), 8u);
    EXPECT_EQ(wgpuTextureGetDimension(color), WGPUTextureDimension_2D);
    EXPECT_EQ(wgpuTextureGetSampleCount(color), 1u);
    EXPECT_EQ(wgpuTextureGetMipLevelCount(color), 1u);
    EXPECT_NE(wgpuTextureGetUsage(color) & WGPUTextureUsage_RenderAttachment, 0u);
    EXPECT_NE(wgpuTextureGetUsage(color) & WGPUTextureUsage_CopySrc, 0u);
    EXPECT_NE(wgpuTextureGetUsage(depth) & WGPUTextureUsage_RenderAttachment, 0u);

    EXPECT_GE(R().GetMaxTextureDimensionForTest(), 2048u);
    EXPECT_GT(R().GetMaxBufferSizeForTest(), 0u);
}

TEST_F(WebGpuTest, PresentWarnsOncePerRendererThatThereIsNoSwapchainYet)
{
    WebGpuRenderer& renderer = NewRenderer();
    ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();

    const auto countWarnings = [](const std::string& output)
    {
        const std::string needle = "Present has nothing to present (no swapchain or window yet)";
        int count = 0;
        for (std::size_t at = output.find(needle); at != std::string::npos; at = output.find(needle, at + 1))
        {
            ++count;
        }
        return count;
    };

    ::testing::internal::CaptureStderr();
    renderer.Present();
    renderer.Present();
    renderer.Present();
    EXPECT_EQ(countWarnings(::testing::internal::GetCapturedStderr()), 1);

    // A re-initialized renderer is a new one: it warns again, once
    ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();
    ::testing::internal::CaptureStderr();
    renderer.Present();
    renderer.Present();
    EXPECT_EQ(countWarnings(::testing::internal::GetCapturedStderr()), 1);
}

TEST_P(WebGpuSizeTest, ClearColourReadsBackExactlyWhenInitializedAtTheSize)
{
    const Size size = GetParam();
    WebGpuRenderer& renderer = NewRenderer();
    ASSERT_TRUE(renderer.Initialize(nullptr, static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)))
        << renderer.GetInitMessage();
    EXPECT_EQ(renderer.GetRenderWidth(), static_cast<uint32_t>(size.w));
    EXPECT_EQ(renderer.GetRenderHeight(), static_cast<uint32_t>(size.h));

    const Rgba colour{10, 200, 30, 255};
    ClearTo(renderer, colour);
    RenderFrame(renderer);
    EXPECT_TRUE(IsSolid(Read(renderer, size.w, size.h), size.w, size.h, colour));
}

TEST_P(WebGpuSizeTest, ClearColourReadsBackExactlyAfterSetRenderTargetSize)
{
    const Size size = GetParam();
    ASSERT_TRUE(R().SetRenderTargetSize(static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)));

    const Rgba colour{255, 1, 128, 77};
    ClearTo(R(), colour);
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, colour));
}

TEST_P(WebGpuSizeTest, ReadbackKeepsEveryPixelAndPutsTheBottomRowFirst)
{
    // The texture's row 0 is the top of the picture; ReadFramebuffer's row 0 is the bottom (as glReadPixels, the software and
    // the Vulkan renderers). A pattern with a different value at every pixel proves the orientation AND that the 256-byte row
    // padding of the copy is removed exactly (a width of 65 pixels is 260 bytes a row, padded to 512; 64 is exactly 256)
    const Size size = GetParam();
    ASSERT_TRUE(R().SetRenderTargetSize(static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)));
    WritePattern(R(), size.w, size.h);

    const std::vector<std::uint8_t> pixels = Read(R(), size.w, size.h);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(size.w) * size.h * 4);
    int wrong = 0;
    for (int y = 0; y < size.h; ++y)
    {
        for (int x = 0; x < size.w; ++x)
        {
            const Rgba expected = PatternPixel(x, size.h - 1 - y); // output row y is texture row h-1-y
            const std::uint8_t* p = &pixels[(static_cast<std::size_t>(y) * size.w + x) * 4];
            if (p[0] != expected.r || p[1] != expected.g || p[2] != expected.b || p[3] != expected.a)
            {
                if (++wrong <= 3)
                {
                    ADD_FAILURE() << "output pixel (" << x << "," << y << ") is (" << int(p[0]) << "," << int(p[1]) << ","
                                  << int(p[2]) << "," << int(p[3]) << "), expected (" << expected.r << "," << expected.g
                                  << "," << expected.b << "," << expected.a << ") of texture row " << (size.h - 1 - y);
                }
            }
        }
    }
    EXPECT_EQ(wrong, 0);
}

INSTANTIATE_TEST_SUITE_P(Sizes, WebGpuSizeTest,
                         ::testing::Values(Size{1, 1}, Size{7, 5}, Size{64, 64}, Size{65, 7}, Size{63, 2}, Size{3, 1},
                                           Size{1, 9}, Size{33, 17}, Size{256, 2}, Size{200, 120}, Size{300, 3}));

TEST_F(WebGpuTest, AFrameReplacesWhatWasWrittenToTheTarget)
{
    // The pattern is written to the target; a frame then clears it (the clear replaces the pattern)
    ASSERT_TRUE(R().SetRenderTargetSize(9, 4));
    WritePattern(R(), 9, 4);
    EXPECT_FALSE(IsSolid(Read(R(), 9, 4), 9, 4, {1, 2, 3, 255}));
    ClearTo(R(), {1, 2, 3, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 9, 4), 9, 4, {1, 2, 3, 255}));
}

TEST_F(WebGpuTest, ReadingAtAnotherSizeResamplesNearestAndKeepsTheOrientation)
{
    // Twice the size: each source pixel becomes a 2x2 block, rows still bottom first (the same arithmetic as the Vulkan renderer's)
    ASSERT_TRUE(R().SetRenderTargetSize(5, 3));
    WritePattern(R(), 5, 3);
    const std::vector<std::uint8_t> big = Read(R(), 10, 6);
    int wrong = 0;
    for (int y = 0; y < 6; ++y)
    {
        for (int x = 0; x < 10; ++x)
        {
            const Rgba expected = PatternPixel(x / 2, 3 - 1 - y / 2);
            const std::uint8_t* p = &big[(static_cast<std::size_t>(y) * 10 + x) * 4];
            if (p[0] != expected.r || p[1] != expected.g || p[2] != expected.b || p[3] != expected.a)
            {
                ++wrong;
            }
        }
    }
    EXPECT_EQ(wrong, 0);

    const Rgba colour{70, 80, 90, 255};
    ClearTo(R(), colour);
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 3, 2), 3, 2, colour));
    EXPECT_TRUE(IsSolid(Read(R(), 16, 16), 16, 16, colour));
    EXPECT_TRUE(IsSolid(Read(R(), 1, 1), 1, 1, colour));
}

TEST_F(WebGpuTest, ClearColourChangesBetweenFrames)
{
    const Rgba colours[] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {0, 0, 0, 0},
                            {255, 255, 255, 255}, {17, 99, 201, 64}, {128, 128, 128, 128}};
    for (const Rgba& colour : colours)
    {
        ClearTo(R(), colour);
        RenderFrame(R());
        EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, colour));
    }
}

TEST_F(WebGpuTest, ManyFramesWithoutReadbackLeaveTheLastOne)
{
    Rgba last{};
    for (int i = 0; i < 30; ++i)
    {
        last = {i * 8, 255 - i * 8, i * 3, 255};
        ClearTo(R(), last);
        RenderFrame(R());
    }
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, last));
}

TEST_F(WebGpuTest, AClearInTheMiddleOfAFrameReplacesTheFrame)
{
    ClearTo(R(), {255, 0, 0, 255});
    R().BeginFrame();
    ClearTo(R(), {0, 0, 255, 255});
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {0, 0, 255, 255}));

    // Twice in one frame: the last one wins
    R().BeginFrame();
    ClearTo(R(), {1, 2, 3, 255});
    ClearTo(R(), {4, 5, 6, 255});
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {4, 5, 6, 255}));
}

TEST_F(WebGpuTest, TheFrameBeforeTheFirstOneReadsZero)
{
    // A new texture is zero, as the software and Vulkan renderers read black before the first frame
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, Black));
}

TEST_F(WebGpuTest, TheDepthTargetIsClearedToOneByEveryFrame)
{
    // Depth behaviour of the core: a frame clears depth to 1.0 (the far plane), whatever the clear colour is. (Depth testing and
    // writing need draws: stage G3a.)
    ASSERT_TRUE(R().SetRenderTargetSize(9, 5));
    ClearTo(R(), {10, 20, 30, 255});
    RenderFrame(R());
    std::vector<float> depth;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ASSERT_TRUE(R().ReadDepthForTest(depth, &width, &height));
    EXPECT_EQ(width, 9u);
    EXPECT_EQ(height, 5u);
    ASSERT_EQ(depth.size(), 45u);
    for (const float value : depth)
    {
        ASSERT_EQ(value, 1.0f);
    }

    // And again after a resize and a clear in the middle of a frame
    ASSERT_TRUE(R().SetRenderTargetSize(65, 3));
    R().BeginFrame();
    ClearTo(R(), {40, 50, 60, 255});
    R().EndFrame();
    ASSERT_TRUE(R().ReadDepthForTest(depth, &width, &height));
    EXPECT_EQ(width, 65u);
    ASSERT_EQ(depth.size(), 65u * 3u);
    for (const float value : depth)
    {
        ASSERT_EQ(value, 1.0f);
    }
}

TEST_F(WebGpuTest, SetRenderTargetSizeThenReadbackAtEachSize)
{
    const Size sizes[] = {{13, 3}, {1, 1}, {64, 64}, {5, 40}, {8, 8}, {70, 2}};
    for (const Size& size : sizes)
    {
        ASSERT_TRUE(R().SetRenderTargetSize(static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)));
        EXPECT_EQ(R().GetRenderWidth(), static_cast<uint32_t>(size.w));
        EXPECT_EQ(R().GetRenderHeight(), static_cast<uint32_t>(size.h));
        EXPECT_EQ(wgpuTextureGetWidth(R().GetColorTextureForTest()), static_cast<uint32_t>(size.w));
        EXPECT_EQ(wgpuTextureGetHeight(R().GetDepthTextureForTest()), static_cast<uint32_t>(size.h));
        // A new target starts zero, then a frame fills it
        EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, Black));
        const Rgba colour{size.w * 3 % 256, size.h * 5 % 256, 40, 255};
        ClearTo(R(), colour);
        RenderFrame(R());
        EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, colour));
    }
}

TEST_F(WebGpuTest, SetRenderTargetSizeRejectsAZeroDimensionAndKeepsTheTarget)
{
    EXPECT_FALSE(R().SetRenderTargetSize(0, 5));
    EXPECT_FALSE(R().SetRenderTargetSize(5, 0));
    EXPECT_FALSE(R().SetRenderTargetSize(0, 0));
    EXPECT_EQ(R().GetRenderWidth(), 8u);
    EXPECT_EQ(R().GetRenderHeight(), 8u);

    EXPECT_TRUE(R().SetRenderTargetSize(8, 8)); // the same size again keeps the target
    ClearTo(R(), {9, 8, 7, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {9, 8, 7, 255}));
}

TEST_F(WebGpuTest, SetRenderTargetSizeOverTheDeviceLimitsFailsAndALaterCallRecovers)
{
    // Over the texture limit: no WebGPU error is raised, the renderer says no first (the buffer limit has its own test)
    const std::uint32_t limit = R().GetMaxTextureDimensionForTest();
    EXPECT_FALSE(R().SetRenderTargetSize(1u << 30, 4));
    EXPECT_EQ(R().GetRenderWidth(), 0u); // the old target is gone
    EXPECT_EQ(R().GetColorTextureForTest(), nullptr);
    ClearTo(R(), {1, 2, 3, 4});
    RenderFrame(R()); // skipped: no target
    EXPECT_FALSE(R().SetRenderTargetSize(limit + 1, 4));
    EXPECT_FALSE(R().SetRenderTargetSize(4, limit + 1));
    EXPECT_TRUE(R().SetRenderTargetSize(6, 4));
    ClearTo(R(), {1, 2, 3, 4});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 6, 4), 6, 4, {1, 2, 3, 4}));
}

TEST_F(WebGpuTest, SetRenderTargetSizeOverTheReadbackBufferLimitFailsWithoutAWebGpuError)
{
    // Within the texture limit, over the (lowered) buffer limit: rows pad to 256 bytes, so 16x16 needs 4096 bytes
    R().SetMaxBufferSizeForTest(1024);
    EXPECT_FALSE(R().SetRenderTargetSize(16, 16));
    EXPECT_EQ(R().GetRenderWidth(), 0u);
    EXPECT_TRUE(R().SetRenderTargetSize(4, 4)); // 4 rows of 256 bytes = exactly 1024: fits
    ClearTo(R(), {7, 8, 9, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 4, 4), 4, 4, {7, 8, 9, 255}));
    EXPECT_FALSE(R().SetRenderTargetSize(4, 5)); // 1280 bytes: one row over
}

TEST_F(WebGpuTest, ResizeFollowsUntilSetRenderTargetSizeFixesTheSize)
{
    R().Resize(20, 10);
    EXPECT_EQ(R().GetRenderWidth(), 20u);
    EXPECT_EQ(R().GetRenderHeight(), 10u);
    R().OnResize(30, 15);
    EXPECT_EQ(R().GetRenderWidth(), 30u);
    EXPECT_EQ(R().GetRenderHeight(), 15u);
    R().OnResize(0, 0); // a minimised window changes nothing
    R().Resize(0, 7);
    EXPECT_EQ(R().GetRenderWidth(), 30u);

    ClearTo(R(), {5, 6, 7, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 30, 15), 30, 15, {5, 6, 7, 255}));

    ASSERT_TRUE(R().SetRenderTargetSize(5, 5));
    R().Resize(40, 40);
    R().OnResize(50, 50);
    EXPECT_EQ(R().GetRenderWidth(), 5u);
    EXPECT_EQ(R().GetRenderHeight(), 5u);
}

TEST_F(WebGpuTest, ResizeChangesTheReadbackSize)
{
    ClearTo(R(), {200, 100, 50, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {200, 100, 50, 255}));
    R().Resize(12, 3);
    ClearTo(R(), {50, 100, 200, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 12, 3), 12, 3, {50, 100, 200, 255}));
}

TEST_F(WebGpuTest, ReadFramebufferIgnoresBadArguments)
{
    std::vector<std::uint8_t> pixels(16, 0xAB);
    R().ReadFramebuffer(nullptr, 2, 2);
    R().ReadFramebuffer(pixels.data(), 0, 2);
    R().ReadFramebuffer(pixels.data(), 2, 0);
    R().ReadFramebuffer(pixels.data(), -2, 2);
    R().ReadFramebuffer(pixels.data(), 2, -2);
    for (const std::uint8_t byte : pixels)
    {
        EXPECT_EQ(byte, 0xAB);
    }
}

TEST_F(WebGpuTest, FrameCallsOutOfOrderAreHarmless)
{
    R().EndFrame(); // no frame open
    R().Present();
    R().BeginFrame();
    R().BeginFrame(); // already open: ignored
    ClearTo(R(), {90, 91, 92, 255});
    R().EndFrame();
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {90, 91, 92, 255}));

    // A frame abandoned by a resize, then a normal one
    R().BeginFrame();
    ASSERT_TRUE(R().SetRenderTargetSize(6, 6));
    R().EndFrame(); // nothing open any more
    ClearTo(R(), {3, 2, 1, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 6, 6), 6, 6, {3, 2, 1, 255}));
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuTest, WhatIsNotSupportedFailsCleanlyAndWarnsOncePerCall)
{
    // Stages G3a and G3b made meshes, textures, materials, the three built-in shaders and draws real (the other
    // WebGpuRenderer*Tests.cpp files). What is left: custom shaders (not supported on any GPU backend) and wireframe (not in core
    // WebGPU). Each returns null or does nothing, says so on stderr ONCE per renderer and call name, and a frame still renders
    // (it clears). Calls with null arguments are harmless
    Renderer::Common::MeshData empty;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    ::testing::internal::CaptureStderr();
    for (int round = 0; round < 3; ++round)
    {
        EXPECT_EQ(R().CreateShaderProgram("void main(){}", "void main(){}"), nullptr);
        EXPECT_FALSE(R().IsValidShader(nullptr));
        EXPECT_FALSE(R().DestroyShaderProgram(nullptr));
        R().UseShaderProgram(nullptr);
        EXPECT_NE(R().GetStandardUnlitShader(), nullptr);
        EXPECT_NE(R().GetStandardLitShader(), nullptr);
        EXPECT_NE(R().GetStandardTextShader(), nullptr);

        EXPECT_EQ(R().CreateMesh(empty), nullptr);
        EXPECT_FALSE(R().UpdateMesh(nullptr, empty));
        R().DestroyMesh(nullptr);
        EXPECT_EQ(R().CreateTexture(nullptr, 1, 1, 4), nullptr);
        R().DestroyTexture(nullptr);
        EXPECT_EQ(R().CreateMaterial(nullptr), nullptr);
        EXPECT_EQ(R().CreateMaterial(nullptr, nullptr), nullptr);
        R().DestroyMaterial(nullptr);

        R().SetViewProjection(identity, identity);
        R().UpdateSceneLighting(Renderer::Common::SceneLightingData{}, N2Engine::Math::Vector3{});
        R().SetWireframe(true);
        ClearTo(R(), {70 + round, 71, 72, 255});
        R().BeginFrame();
        R().DrawMesh(nullptr, identity, nullptr);
        R().DrawMesh(nullptr, identity, nullptr, Renderer::Common::RenderState{});
        R().DrawMesh(nullptr, identity, nullptr, Renderer::Common::RenderState{}, Renderer::Common::IndexRange{0, 3});
        R().DrawObjects({});
        R().EndFrame();
        EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {70 + round, 71, 72, 255}));
    }
    const std::string output = ::testing::internal::GetCapturedStderr();

    const auto count = [&output](const std::string& needle)
    {
        int found = 0;
        for (std::size_t at = output.find(needle); at != std::string::npos; at = output.find(needle, at + 1))
        {
            ++found;
        }
        return found;
    };
    EXPECT_EQ(count("is not implemented yet"), 0) << output;
    EXPECT_EQ(count("WebGPU renderer: CreateShaderProgram: custom shaders are not supported"), 1);
    EXPECT_EQ(count("WebGPU renderer: SetWireframe: wireframe is not supported"), 1);
}
