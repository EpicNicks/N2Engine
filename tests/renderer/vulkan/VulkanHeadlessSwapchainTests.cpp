#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IMesh.hpp>
#include <renderer/common/RenderState.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>
#include <renderer/vulkan/VulkanSwapchainChoice.hpp>

#include "VulkanDrawTestUtil.hpp"
#include "VulkanTestFixture.hpp"

// The Vulkan backend's swapchain path (stage F of #43) without a window: a VK_EXT_headless_surface surface stands in for
// the window (VulkanRenderer::SetHeadlessSurfaceForTest), and a test-settable "framebuffer size" for
// glfwGetFramebufferSize, so resize and minimise (0x0) can be simulated. Everything Present does with a window runs: the
// surface, the swapchain and its recreation, the acquire, the blit and the present, with validation on (zero errors and
// warnings, no leaks, as in every test of tests/renderer/vulkan/). What a headless surface cannot show (a real compositor,
// real present modes, HiDPI, a real driver) is the GPU checklist's (docs/gpu-vulkan-checklist.html).
//
// The extension is optional: where the driver does not offer it these tests SKIP with that reason, even under
// N2_REQUIRE_VULKAN=1 (it is not part of the driver contract the other tests rely on). N2_REQUIRE_VULKAN_HEADLESS_SURFACE=1
// turns that skip into a failure; the Linux Clang CI legs set it, so the swapchain tests cannot silently stop running there.

using Renderer::Vulkan::SwapchainCallForTest;
using Renderer::Vulkan::SwapchainInfoForTest;
using Renderer::Vulkan::VulkanRenderer;
using namespace VulkanTesting;

namespace
{
    std::string Describe(const SwapchainInfoForTest& info)
    {
        std::ostringstream text;
        text << "format " << info.format.format << " (colour space " << info.format.colorSpace << "), present mode "
             << info.presentMode << ", " << info.imageCount << " images (asked for " << info.requestedImageCount
             << "), extent " << info.extent.width << "x" << info.extent.height << "; surface: minImageCount "
             << info.capabilities.minImageCount << ", maxImageCount " << info.capabilities.maxImageCount
             << ", currentExtent " << info.capabilities.currentExtent.width << "x"
             << info.capabilities.currentExtent.height << "; offered formats:";
        for (const VkSurfaceFormatKHR& format : info.offeredFormats)
        {
            text << " " << format.format << "/" << format.colorSpace;
        }
        text << "; offered present modes:";
        for (const VkPresentModeKHR mode : info.offeredPresentModes)
        {
            text << " " << mode;
        }
        return text.str();
    }

    class VulkanHeadlessSwapchainTest : public VulkanTest
    {
    protected:
        void SetUp() override
        {
            VulkanTest::SetUp();
            if (HasFatalFailure() || IsSkipped())
            {
                return;
            }
            const bool available = VulkanRenderer::IsHeadlessSurfaceAvailableForTest();
            std::cout << "[ vulkan ] VK_EXT_headless_surface: " << (available ? "offered" : "NOT offered") << std::endl;
            if (!available)
            {
                if (Env("N2_REQUIRE_VULKAN_HEADLESS_SURFACE") == "1")
                {
                    FAIL() << "N2_REQUIRE_VULKAN_HEADLESS_SURFACE=1 but VK_EXT_headless_surface is not offered by this "
                              "driver: the swapchain path was not executed";
                }
                GTEST_SKIP() << "VK_EXT_headless_surface not offered by this driver: the swapchain path was not executed";
            }
        }

        /// A renderer with a headless surface whose "window framebuffer" is width x height, and an offscreen target of
        /// the same size (1x1 for a 0 dimension); initialized, or the test has failed
        VulkanRenderer& Windowed(uint32_t width, uint32_t height, bool capture = false, bool vsync = true)
        {
            VulkanRenderer& renderer = NewRenderer();
            renderer.SetHeadlessSurfaceForTest(true, width, height);
            renderer.SetPresentCaptureForTest(capture);
            renderer.SetVSync(vsync);
            const bool initialized = renderer.Initialize(nullptr, std::max(width, 1u), std::max(height, 1u));
            EXPECT_TRUE(initialized) << "status " << static_cast<int>(renderer.GetInitStatus()) << ": "
                                     << renderer.GetInitMessage();
            return renderer;
        }

        static void Frame(VulkanRenderer& renderer, const Rgba& colour)
        {
            ClearTo(renderer, colour);
            renderer.BeginFrame();
            renderer.EndFrame();
            renderer.Present();
        }
    };
}

TEST_F(VulkanHeadlessSwapchainTest, CreatesTheSwapchainWithTheDocumentedChoices)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    const SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    std::cout << "[ vulkan ] headless swapchain: " << Describe(info) << std::endl;
    ASSERT_TRUE(info.hasSurface);
    ASSERT_TRUE(info.hasSwapchain);
    EXPECT_FALSE(info.minimised);
    EXPECT_FALSE(info.presentingStopped);

    const Renderer::Vulkan::SurfaceFormatChoice expected = Renderer::Vulkan::ChooseSurfaceFormat(info.offeredFormats);
    ASSERT_TRUE(expected.found);
    EXPECT_EQ(info.format.format, expected.format.format);
    EXPECT_EQ(info.format.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    if (!expected.doubleEncoded)
    {
        EXPECT_FALSE(Renderer::Vulkan::IsSrgbFormat(info.format.format)) << "a UNORM format was offered";
    }
    EXPECT_TRUE(renderer.GetVSync());
    EXPECT_EQ(info.presentMode, VK_PRESENT_MODE_FIFO_KHR) << "vsync on presents with FIFO";
    EXPECT_EQ(info.requestedImageCount, Renderer::Vulkan::ChooseImageCount(info.capabilities));
    EXPECT_GE(info.imageCount, info.requestedImageCount);
    const Renderer::Vulkan::ExtentChoice extent = Renderer::Vulkan::ChooseExtent(info.capabilities, 64, 48);
    EXPECT_EQ(info.extent.width, extent.extent.width);
    EXPECT_EQ(info.extent.height, extent.extent.height);
    EXPECT_EQ(info.createCount, 1u);
    EXPECT_EQ(info.surfaceCreateCount, 1u);
    EXPECT_EQ(info.presentCount, 0u);

    // The device chosen for the surface presents to it, and is still lavapipe in CI
    int supported = 0;
    for (const auto& gpu : renderer.GetCompatibleGPUs())
    {
        if (renderer.SelectGPU(gpu.device))
        {
            EXPECT_TRUE(gpu.isSupported);
            ++supported;
        }
    }
    EXPECT_EQ(supported, 1);
    if (Env("N2_VULKAN_EXPECT_CPU_DEVICE") == "1")
    {
        EXPECT_EQ(renderer.GetDeviceType(), VK_PHYSICAL_DEVICE_TYPE_CPU);
    }
}

TEST_F(VulkanHeadlessSwapchainTest, ManyFramesWithDrawsPresentValidationClean)
{
    N2_REQUIRE_BUILT_IN_SHADERS();
    VulkanRenderer& renderer = Windowed(64, 64);
    ASSERT_FALSE(HasFailure());
    Renderer::Common::IMaterial* material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(material, nullptr);
    Renderer::Common::IMesh* mesh = renderer.CreateMesh(Draw::Quad(-0.5f, -0.5f, 0.5f, 0.5f, 0.0f));
    ASSERT_NE(mesh, nullptr);

    // More frames than twice the frames in flight and than the swapchain's images, so every slot, semaphore and image is
    // reused several times
    const uint32_t images = renderer.GetSwapchainInfoForTest().imageCount;
    const uint32_t frames = std::max(12u, images * 3);
    for (uint32_t i = 0; i < frames; ++i)
    {
        ClearTo(renderer, {static_cast<int>(i * 7 % 256), 40, 80, 255});
        material->SetColor("uAlbedo", 1.0f, static_cast<float>(i % 2), 0.0f, 1.0f);
        renderer.BeginFrame();
        renderer.SetViewProjection(Draw::Identity, Draw::Identity);
        renderer.DrawMesh(mesh, Draw::Identity, material, Renderer::Common::RenderState::Opaque());
        renderer.EndFrame();
        renderer.Present();
    }
    const SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.presentCount, frames);
    EXPECT_EQ(info.createCount, 1u) << "nothing asked for a new swapchain";

    // Present needs a submitted frame: a second Present of the same frame does nothing
    renderer.Present();
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, frames);

    // The offscreen frame (what was presented) still reads back
    const std::vector<std::uint8_t> pixels = Read(renderer, 64, 64);
    const std::size_t centre = (32u * 64u + 32u) * 4u;
    EXPECT_EQ(pixels[centre + 0], 255);
    EXPECT_EQ(pixels[centre + 1], static_cast<std::uint8_t>(((frames - 1) % 2) * 255));
    EXPECT_EQ(pixels[0 + 1], 40) << "the clear colour in a corner";
}

TEST_F(VulkanHeadlessSwapchainTest, ThePresentedImageHoldsReadFramebuffersPixelsTopRowFirst)
{
    N2_REQUIRE_BUILT_IN_SHADERS();
    constexpr int W = 32;
    constexpr int H = 24;
    VulkanRenderer& renderer = Windowed(W, H, true);
    ASSERT_FALSE(HasFailure());
    Renderer::Common::IMaterial* material = renderer.CreateMaterial(renderer.GetStandardUnlitShader());
    ASSERT_NE(material, nullptr);
    material->SetColor("uAlbedo", Unorm(10), Unorm(220), Unorm(30), 1.0f);
    // The top half of the picture (NDC y 0 to 1), over a clear whose red and blue differ, so a flip or an R/B swap shows
    Renderer::Common::IMesh* mesh = renderer.CreateMesh(Draw::Quad(-1.0f, 0.0f, 1.0f, 1.0f, 0.0f));
    ASSERT_NE(mesh, nullptr);
    ClearTo(renderer, {200, 10, 60, 255});
    renderer.BeginFrame();
    renderer.SetViewProjection(Draw::Identity, Draw::Identity);
    renderer.DrawMesh(mesh, Draw::Identity, material, Renderer::Common::RenderState::Opaque());
    renderer.EndFrame();
    renderer.Present();

    std::vector<std::uint8_t> presented;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!renderer.ReadPresentedImageForTest(presented, width, height))
    {
        GTEST_SKIP() << "the surface does not let the swapchain images be copied (TRANSFER_SRC) or its format is not an "
                        "8-bit one: the presented pixels were not checked ("
                     << Describe(renderer.GetSwapchainInfoForTest()) << ")";
    }
    ASSERT_EQ(width, static_cast<uint32_t>(W));
    ASSERT_EQ(height, static_cast<uint32_t>(H));
    const std::vector<std::uint8_t> frame = Read(renderer, W, H); // bottom row first

    // The top row of the window is the quad, the bottom row the clear colour, in RGBA order
    EXPECT_EQ(presented[0], 10);
    EXPECT_EQ(presented[1], 220);
    EXPECT_EQ(presented[2], 30);
    const std::size_t bottom = static_cast<std::size_t>(H - 1) * W * 4;
    EXPECT_EQ(presented[bottom + 0], 200);
    EXPECT_EQ(presented[bottom + 1], 10);
    EXPECT_EQ(presented[bottom + 2], 60);

    // Every pixel equals ReadFramebuffer's, with the row order flipped (an unscaled blit is a copy)
    int mismatches = 0;
    for (int y = 0; y < H && mismatches < 5; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const std::size_t top = (static_cast<std::size_t>(y) * W + x) * 4;
            const std::size_t fromBottom = (static_cast<std::size_t>(H - 1 - y) * W + x) * 4;
            for (int c = 0; c < 4; ++c)
            {
                if (presented[top + c] != frame[fromBottom + c])
                {
                    ADD_FAILURE() << "presented pixel (" << x << "," << y << ") channel " << c << " is "
                                  << int(presented[top + c]) << ", ReadFramebuffer has " << int(frame[fromBottom + c]);
                    ++mismatches;
                    break;
                }
            }
        }
    }
}

TEST_F(VulkanHeadlessSwapchainTest, AResizeRecreatesTheSwapchainAtTheNewExtentAndTheTargetFollows)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    Frame(renderer, {1, 2, 3, 255});
    ASSERT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 1u);

    // A window resize: the callback reports the new framebuffer size
    renderer.SetFramebufferSizeForTest(100, 60);
    renderer.OnResize(100, 60);
    EXPECT_EQ(renderer.GetRenderWidth(), 100u) << "the offscreen target follows the window";
    EXPECT_EQ(renderer.GetRenderHeight(), 60u);
    Frame(renderer, {4, 5, 6, 255});
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.createCount, 2u);
    EXPECT_EQ(info.presentCount, 2u);
    Renderer::Vulkan::ExtentChoice expected = Renderer::Vulkan::ChooseExtent(info.capabilities, 100, 60);
    EXPECT_EQ(info.extent.width, expected.extent.width);
    EXPECT_EQ(info.extent.height, expected.extent.height);
    EXPECT_TRUE(IsSolid(Read(renderer, 100, 60), 100, 60, {4, 5, 6, 255}));

    // A size change without the callback (Window skips OnResize while SetRenderSize fixes the size) is noticed by Present
    renderer.SetFramebufferSizeForTest(80, 80);
    Frame(renderer, {7, 8, 9, 255});
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.createCount, 3u);
    EXPECT_EQ(info.presentCount, 3u);
    expected = Renderer::Vulkan::ChooseExtent(info.capabilities, 80, 80);
    EXPECT_EQ(info.extent.width, expected.extent.width);
    EXPECT_EQ(renderer.GetRenderWidth(), 100u) << "no OnResize: the offscreen target stays, the blit scales it";

    // The same size again recreates nothing
    Frame(renderer, {7, 8, 9, 255});
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().createCount, 3u);
}

TEST_F(VulkanHeadlessSwapchainTest, AFixedRenderTargetSizeIsBlitScaledToTheWindow)
{
    constexpr uint32_t WindowW = 96;
    constexpr uint32_t WindowH = 64;
    VulkanRenderer& renderer = Windowed(WindowW, WindowH, true);
    ASSERT_FALSE(HasFailure());
    ASSERT_TRUE(renderer.SetRenderTargetSize(32, 16)); // the editor viewport's case: a fixed size, any window
    Frame(renderer, {40, 120, 200, 255});
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.presentCount, 1u);
    EXPECT_EQ(renderer.GetRenderWidth(), 32u);

    std::vector<std::uint8_t> presented;
    uint32_t width = 0;
    uint32_t height = 0;
    if (renderer.ReadPresentedImageForTest(presented, width, height))
    {
        EXPECT_EQ(width, info.extent.width);
        EXPECT_EQ(height, info.extent.height);
        // A uniform frame stays uniform when stretched, whatever the filter
        int wrong = 0;
        for (std::size_t i = 0; i < presented.size(); i += 4)
        {
            wrong += (presented[i] != 40 || presented[i + 1] != 120 || presented[i + 2] != 200) ? 1 : 0;
        }
        EXPECT_EQ(wrong, 0) << "of " << presented.size() / 4 << " presented pixels";
    }
    else
    {
        std::cout << "[ vulkan ] the presented pixels of the scaled blit were not checked (no capture on this surface)"
                  << std::endl;
    }

    // The window resizes; the target keeps its fixed size
    renderer.SetFramebufferSizeForTest(50, 70);
    renderer.OnResize(50, 70);
    Frame(renderer, {40, 120, 200, 255});
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(renderer.GetRenderWidth(), 32u);
    EXPECT_EQ(renderer.GetRenderHeight(), 16u);
    EXPECT_EQ(info.createCount, 2u);
    EXPECT_EQ(info.presentCount, 2u);
}

TEST_F(VulkanHeadlessSwapchainTest, MinimisedFramesStillRenderButNothingIsRecreatedOrPresented)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    Frame(renderer, {10, 10, 10, 255});
    ASSERT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 1u);

    renderer.SetFramebufferSizeForTest(0, 0);
    ::testing::internal::CaptureStderr();
    renderer.OnResize(0, 0);
    for (int i = 0; i < 6; ++i)
    {
        Frame(renderer, {20, 30, static_cast<int>(40 + i), 255});
    }
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(output, "") << "minimised frames log nothing";
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.presentCount, 1u) << "nothing presented while minimised";
    EXPECT_EQ(info.createCount, 1u) << "no swapchain made while minimised";
    EXPECT_EQ(renderer.GetRenderWidth(), 64u) << "the offscreen target keeps its size";
    EXPECT_TRUE(IsSolid(Read(renderer, 64, 48), 64, 48, {20, 30, 45, 255})) << "frames still render and read back";

    // Restored: a new swapchain, and presenting again
    renderer.SetFramebufferSizeForTest(64, 48);
    renderer.OnResize(64, 48);
    Frame(renderer, {50, 60, 70, 255});
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.createCount, 2u);
    EXPECT_EQ(info.presentCount, 2u);
    EXPECT_FALSE(info.minimised);
}

TEST_F(VulkanHeadlessSwapchainTest, AWindowMinimisedAtInitializeGetsItsSwapchainWhenRestored)
{
    VulkanRenderer& renderer = Windowed(0, 0);
    ASSERT_FALSE(HasFailure());
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_TRUE(info.hasSurface);
    EXPECT_FALSE(info.hasSwapchain) << "no swapchain at a 0x0 size";
    EXPECT_TRUE(info.minimised);
    Frame(renderer, {1, 1, 1, 255});
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 0u);

    renderer.SetFramebufferSizeForTest(40, 30);
    renderer.OnResize(40, 30);
    Frame(renderer, {2, 2, 2, 255});
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_TRUE(info.hasSwapchain);
    EXPECT_FALSE(info.minimised);
    EXPECT_EQ(info.createCount, 1u);
    EXPECT_EQ(info.presentCount, 1u);
}

TEST_F(VulkanHeadlessSwapchainTest, OutOfDateAndSuboptimalFromAcquireAndPresentRecreateAsDocumented)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    Frame(renderer, {1, 0, 0, 255});
    uint32_t presents = 1;
    uint32_t creates = 1;
    const auto check = [&renderer](uint32_t expectedPresents, uint32_t expectedCreates, const char* step)
    {
        const SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
        EXPECT_EQ(info.presentCount, expectedPresents) << step;
        EXPECT_EQ(info.createCount, expectedCreates) << step;
        EXPECT_TRUE(info.hasSwapchain) << step;
    };
    check(presents, creates, "start");

    // OUT_OF_DATE from the acquire: no image, so this frame is not presented; recreated at once
    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Acquire, VK_ERROR_OUT_OF_DATE_KHR);
    Frame(renderer, {2, 0, 0, 255});
    check(presents, ++creates, "acquire OUT_OF_DATE");
    Frame(renderer, {3, 0, 0, 255});
    check(++presents, creates, "the frame after");

    // SUBOPTIMAL from the acquire: the image is presented, and the swapchain recreated before the next present
    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Acquire, VK_SUBOPTIMAL_KHR);
    Frame(renderer, {4, 0, 0, 255});
    check(++presents, creates, "acquire SUBOPTIMAL");
    Frame(renderer, {5, 0, 0, 255});
    check(++presents, ++creates, "the frame after acquire SUBOPTIMAL");

    // OUT_OF_DATE from the present: the present was made; recreated before the next
    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Present, VK_ERROR_OUT_OF_DATE_KHR);
    Frame(renderer, {6, 0, 0, 255});
    check(++presents, creates, "present OUT_OF_DATE");
    Frame(renderer, {7, 0, 0, 255});
    check(++presents, ++creates, "the frame after present OUT_OF_DATE");

    // SUBOPTIMAL from the present: the same
    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Present, VK_SUBOPTIMAL_KHR);
    Frame(renderer, {8, 0, 0, 255});
    check(++presents, creates, "present SUBOPTIMAL");
    Frame(renderer, {9, 0, 0, 255});
    check(++presents, ++creates, "the frame after present SUBOPTIMAL");

    EXPECT_TRUE(IsSolid(Read(renderer, 64, 48), 64, 48, {9, 0, 0, 255}));
}

TEST_F(VulkanHeadlessSwapchainTest, ALostSurfaceIsMadeAgainOnceAndPresentingGoesOn)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    Frame(renderer, {1, 1, 1, 255});

    // From the acquire: nothing presented this frame; a new surface and swapchain
    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Acquire, VK_ERROR_SURFACE_LOST_KHR);
    ::testing::internal::CaptureStderr();
    Frame(renderer, {2, 2, 2, 255});
    std::string output = ::testing::internal::GetCapturedStderr();
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.surfaceCreateCount, 2u);
    EXPECT_EQ(info.createCount, 2u);
    EXPECT_EQ(info.presentCount, 1u);
    EXPECT_FALSE(info.presentingStopped);
    EXPECT_NE(output.find("surface was lost"), std::string::npos) << output;
    Frame(renderer, {3, 3, 3, 255});
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 2u);

    // From the present: the present was made; then a new surface and swapchain
    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Present, VK_ERROR_SURFACE_LOST_KHR);
    Frame(renderer, {4, 4, 4, 255});
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.surfaceCreateCount, 3u);
    EXPECT_EQ(info.createCount, 3u);
    EXPECT_EQ(info.presentCount, 3u);
    Frame(renderer, {5, 5, 5, 255});
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 4u);
    EXPECT_TRUE(IsSolid(Read(renderer, 64, 48), 64, 48, {5, 5, 5, 255}));
}

TEST_F(VulkanHeadlessSwapchainTest, ALostDeviceStopsTheFramesAndShutdownStillCleansUp)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    Frame(renderer, {1, 1, 1, 255});

    renderer.ForceNextSwapchainResultForTest(SwapchainCallForTest::Acquire, VK_ERROR_DEVICE_LOST);
    ::testing::internal::CaptureStderr();
    Frame(renderer, {2, 2, 2, 255});
    Frame(renderer, {3, 3, 3, 255}); // skipped
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), 1) << "one log line: " << output;
    EXPECT_NE(output.find("the device was lost"), std::string::npos) << output;
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 1u);

    std::vector<std::uint8_t> pixels(16, 0xAB);
    renderer.ReadFramebuffer(pixels.data(), 2, 2);
    EXPECT_EQ(pixels[0], 0xAB) << "frames are skipped after a lost device";
    EXPECT_FALSE(renderer.SetRenderTargetSize(8, 8));
    renderer.Shutdown(); // the fixture checks it leaked nothing and reported nothing
    EXPECT_FALSE(renderer.GetSwapchainInfoForTest().hasSurface);
}

TEST_F(VulkanHeadlessSwapchainTest, VsyncOffPicksMailboxThenImmediateThenFifo)
{
    VulkanRenderer& renderer = Windowed(64, 48);
    ASSERT_FALSE(HasFailure());
    Frame(renderer, {1, 1, 1, 255});
    const std::vector<VkPresentModeKHR> offered = renderer.GetSwapchainInfoForTest().offeredPresentModes;

    renderer.SetVSync(false); // takes effect at the next Present: the swapchain is recreated
    EXPECT_FALSE(renderer.GetVSync());
    Frame(renderer, {2, 2, 2, 255});
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.createCount, 2u);
    EXPECT_EQ(info.presentMode, Renderer::Vulkan::ChoosePresentMode(offered, false));
    std::cout << "[ vulkan ] vsync off on the headless surface: present mode " << info.presentMode << std::endl;
    EXPECT_EQ(info.presentCount, 2u);

    renderer.SetVSync(true);
    Frame(renderer, {3, 3, 3, 255});
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_EQ(info.createCount, 3u);
    EXPECT_EQ(info.presentMode, VK_PRESENT_MODE_FIFO_KHR);

    renderer.SetVSync(true); // no change: no recreate
    Frame(renderer, {4, 4, 4, 255});
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().createCount, 3u);

    // Set before Initialize, the first swapchain already uses it
    VulkanRenderer& early = Windowed(32, 32, false, false);
    ASSERT_FALSE(HasFailure());
    EXPECT_EQ(early.GetSwapchainInfoForTest().presentMode, Renderer::Vulkan::ChoosePresentMode(offered, false));
    Frame(early, {5, 5, 5, 255});
    EXPECT_EQ(early.GetSwapchainInfoForTest().presentCount, 1u);
}

TEST_F(VulkanHeadlessSwapchainTest, ReinitializeAndShutdownWithASwapchainLeakNothing)
{
    VulkanRenderer& renderer = Windowed(64, 48, true);
    ASSERT_FALSE(HasFailure());
    for (int i = 0; i < 5; ++i)
    {
        Frame(renderer, {i, 1, 1, 255});
    }

    // Initialize again without Shutdown: the old surface goes with the old instance before the new one is made
    ASSERT_TRUE(renderer.Initialize(nullptr, 32, 32)) << renderer.GetInitMessage();
    SwapchainInfoForTest info = renderer.GetSwapchainInfoForTest();
    EXPECT_TRUE(info.hasSwapchain);
    EXPECT_EQ(info.surfaceCreateCount, 1u);
    EXPECT_EQ(info.createCount, 1u);
    EXPECT_EQ(info.presentCount, 0u);
    for (int i = 0; i < 5; ++i)
    {
        Frame(renderer, {i, 2, 2, 255});
    }
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 5u);

    renderer.Shutdown();
    info = renderer.GetSwapchainInfoForTest();
    EXPECT_FALSE(info.hasSurface);
    EXPECT_FALSE(info.hasSwapchain);
    renderer.Present(); // harmless after Shutdown

    ASSERT_TRUE(renderer.Initialize(nullptr, 16, 16)) << renderer.GetInitMessage();
    Frame(renderer, {9, 9, 9, 255});
    EXPECT_EQ(renderer.GetSwapchainInfoForTest().presentCount, 1u);
    // The fixture's TearDown shuts it down and fails the test on any leaked object
}
