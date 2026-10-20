#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "WebGpuTestFixture.hpp"

// The WebGPU backend's error counting, device loss, adapter selection, shutdown and leak checks (#155, stage G2). The core
// (init, clear, readback, resize) is in WebGpuRendererTests.cpp. Linux only, like the whole harness (POSIX setenv and /proc).

using Renderer::WebGpu::AdapterPreference;
using Renderer::WebGpu::InitStatus;
using Renderer::WebGpu::WebGpuRenderer;
using namespace WebGpuTesting; // the fixture and helpers

namespace
{
    /// Sets an environment variable for the life of the object, then puts it back
    class ScopedEnvironment
    {
    public:
        ScopedEnvironment(const char* name, const char* value) : _name(name)
        {
            if (const char* old = std::getenv(name); old != nullptr)
            {
                _old = std::string(old);
            }
            ::setenv(name, value, 1);
        }
        ~ScopedEnvironment()
        {
            if (_old.has_value())
            {
                ::setenv(_name.c_str(), _old->c_str(), 1);
            }
            else
            {
                ::unsetenv(_name.c_str());
            }
        }
        ScopedEnvironment(const ScopedEnvironment&) = delete;
        ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    private:
        std::string _name;
        std::optional<std::string> _old;
    };

    /// The resident set of this process in bytes (Linux /proc/self/statm), 0 when unreadable
    std::uint64_t ResidentBytes()
    {
        std::ifstream statm("/proc/self/statm");
        std::uint64_t size = 0;
        std::uint64_t resident = 0;
        if (!(statm >> size >> resident))
        {
            return 0;
        }
        return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    }

    /// Makes the device report an error on purpose, through the raw device: a buffer with no usage is a validation error
    void ProvokeValidationError(const WebGpuRenderer& renderer)
    {
        WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        descriptor.size = 16;
        descriptor.usage = WGPUBufferUsage_None;
        WGPUBuffer buffer = wgpuDeviceCreateBuffer(renderer.GetDeviceForTest(), &descriptor);
        if (buffer != nullptr)
        {
            wgpuBufferRelease(buffer); // an error buffer: still a reference to give back
        }
    }
}

TEST_F(WebGpuTest, ErrorCounterNegativeControlADeliberateValidationErrorIsCountedAndKept)
{
    // Proves the zero-errors assertion of every other test can fail: an error made on purpose through the raw device must be
    // counted (after the renderer made the device deliver it), kept with its text, reported by the fixture's report, and
    // survive until the test resets it
    ASSERT_EQ(R().GetValidationErrorCount(), 0u);
    EXPECT_TRUE(ValidationReport(R()).empty());

    ProvokeValidationError(R());
    const std::uint32_t afterOne = R().GetValidationErrorCount();
    EXPECT_GE(afterOne, 1u) << "the validation error was not counted";
    bool kept = false;
    for (const auto& message : R().GetValidationMessages())
    {
        kept = kept || message.find("validation error") != std::string::npos;
    }
    EXPECT_TRUE(kept) << "the error text was not retained";
    EXPECT_FALSE(ValidationReport(R()).empty()) << "the report that fails every test is empty after an error";

    ProvokeValidationError(R());
    EXPECT_GT(R().GetValidationErrorCount(), afterOne) << "a second error was not counted";

    // An error of another call, again through the raw device: a texture of no size
    const std::uint32_t beforeTexture = R().GetValidationErrorCount();
    WGPUTextureDescriptor texture = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texture.usage = WGPUTextureUsage_RenderAttachment;
    texture.dimension = WGPUTextureDimension_2D;
    texture.size = {0, 0, 1};
    texture.format = WGPUTextureFormat_RGBA8Unorm;
    WGPUTexture bad = wgpuDeviceCreateTexture(R().GetDeviceForTest(), &texture);
    if (bad != nullptr)
    {
        wgpuTextureRelease(bad);
    }
    EXPECT_GT(R().GetValidationErrorCount(), beforeTexture) << "an error of another call was not counted";

    // The renderer still works, and the counters reset, so this test's own teardown (zero errors) passes
    ClearTo(R(), {5, 6, 7, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {5, 6, 7, 255}));
    R().ResetValidationErrors();
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
    EXPECT_TRUE(R().GetValidationMessages().empty());
    EXPECT_TRUE(ValidationReport(R()).empty());
}

TEST_F(WebGpuTest, TheErrorCountSurvivesShutdownAndInitializeAgain)
{
    ProvokeValidationError(R());
    ASSERT_GE(R().GetValidationErrorCount(), 1u);
    const std::uint32_t count = R().GetValidationErrorCount();

    R().Shutdown();
    EXPECT_EQ(R().GetValidationErrorCount(), count);
    ASSERT_TRUE(R().Initialize(nullptr, 4, 4)) << R().GetInitMessage();
    EXPECT_EQ(R().GetValidationErrorCount(), count) << "one number covers the whole life of the renderer object";

    R().ResetValidationErrors();
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuTest, ALostDeviceIsCountedAsAnErrorAndFramesAreSkippedUntilANewInitialize)
{
    ASSERT_FALSE(R().IsDeviceLost());
    ASSERT_EQ(R().GetValidationErrorCount(), 0u);

    // Destroying the device behind the renderer's back is a loss the renderer did not cause
    wgpuDeviceDestroy(R().GetDeviceForTest());
    EXPECT_GE(R().GetValidationErrorCount(), 1u) << "the device loss was not counted";
    EXPECT_EQ(R().GetDeviceLostCount(), 1u);
    EXPECT_TRUE(R().IsDeviceLost());
    bool kept = false;
    for (const auto& message : R().GetValidationMessages())
    {
        kept = kept || message.find("device lost") != std::string::npos;
    }
    EXPECT_TRUE(kept) << "the loss was not recorded in the messages";
    EXPECT_FALSE(ValidationReport(R()).empty());

    // Nothing crashes, nothing is read, no size is accepted
    RenderFrame(R());
    R().Resize(9, 9);
    EXPECT_FALSE(R().SetRenderTargetSize(9, 9));
    std::vector<std::uint8_t> pixels(16, 0xAB);
    R().ReadFramebuffer(pixels.data(), 2, 2);
    for (const std::uint8_t byte : pixels)
    {
        EXPECT_EQ(byte, 0xAB);
    }

    // A new Initialize makes a new device, and the renderer works again
    ASSERT_TRUE(R().Initialize(nullptr, 5, 5)) << R().GetInitMessage();
    EXPECT_FALSE(R().IsDeviceLost());
    ClearTo(R(), {11, 22, 33, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 5, 5), 5, 5, {11, 22, 33, 255}));

    R().ResetValidationErrors(); // this test provoked them on purpose
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
    EXPECT_EQ(R().GetDeviceLostCount(), 0u);
}

TEST_F(WebGpuTest, TheRenderersOwnShutdownIsNotADeviceLoss)
{
    R().Shutdown();
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
    EXPECT_EQ(R().GetDeviceLostCount(), 0u);
    EXPECT_FALSE(R().IsDeviceLost());
    ASSERT_TRUE(R().Initialize(nullptr, 3, 3)) << R().GetInitMessage();
    R().Shutdown();
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuTest, ReinitializeAfterShutdownMakesANewRenderer)
{
    const std::weak_ptr<const void> first = R().GetLifetimeToken();
    EXPECT_FALSE(first.expired());
    R().Shutdown();
    EXPECT_TRUE(first.expired());
    EXPECT_EQ(R().GetRenderWidth(), 0u);
    EXPECT_EQ(R().GetInitStatus(), InitStatus::NotInitialized);
    EXPECT_EQ(R().GetDeviceForTest(), nullptr);
    EXPECT_EQ(R().GetColorTextureForTest(), nullptr);
    EXPECT_TRUE(R().GetAdapterName().empty());

    const std::pair<int, int> sizes[] = {{5, 3}, {16, 16}, {1, 2}};
    for (const std::pair<int, int>& size : sizes)
    {
        ASSERT_TRUE(R().Initialize(nullptr, static_cast<uint32_t>(size.first), static_cast<uint32_t>(size.second)))
            << R().GetInitMessage();
        EXPECT_EQ(R().GetInitStatus(), InitStatus::Ok);
        const std::weak_ptr<const void> token = R().GetLifetimeToken();
        EXPECT_FALSE(token.expired());
        EXPECT_TRUE(first.expired()); // a new renderer, not the old one back

        const Rgba colour{size.first * 7 % 256, 100, size.second * 11 % 256, 255};
        ClearTo(R(), colour);
        RenderFrame(R());
        EXPECT_TRUE(IsSolid(Read(R(), size.first, size.second), size.first, size.second, colour));

        R().Shutdown();
        EXPECT_TRUE(token.expired());
    }
}

TEST_F(WebGpuTest, InitializeAgainWithoutShutdownReplacesTheRenderer)
{
    const std::weak_ptr<const void> token = R().GetLifetimeToken();
    ASSERT_TRUE(R().Initialize(nullptr, 4, 4)) << R().GetInitMessage();
    EXPECT_TRUE(token.expired());
    ClearTo(R(), {200, 100, 50, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 4, 4), 4, 4, {200, 100, 50, 255}));
}

TEST_F(WebGpuTest, ShutdownTwiceIsSafeAndEverythingAfterItIsHarmless)
{
    R().Shutdown();
    R().Shutdown();

    RenderFrame(R());
    R().BeginFrame();
    R().Clear(1, 1, 1, 1);
    R().EndFrame();
    R().Resize(9, 9);
    EXPECT_FALSE(R().SetRenderTargetSize(9, 9));
    std::vector<std::uint8_t> pixels(16, 0xAB);
    R().ReadFramebuffer(pixels.data(), 2, 2);
    for (const std::uint8_t byte : pixels)
    {
        EXPECT_EQ(byte, 0xAB); // nothing written
    }
    EXPECT_EQ(R().GetRenderWidth(), 0u);
    EXPECT_EQ(R().GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuTest, ARendererThatWasNeverInitializedIsHarmless)
{
    WebGpuRenderer& renderer = NewRenderer();
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::NotInitialized);
    RenderFrame(renderer);
    EXPECT_FALSE(renderer.SetRenderTargetSize(4, 4));
    std::vector<std::uint8_t> pixels(16, 0xAB);
    renderer.ReadFramebuffer(pixels.data(), 2, 2);
    EXPECT_EQ(pixels[0], 0xAB);
    EXPECT_EQ(renderer.GetValidationErrorCount(), 0u);
    renderer.Shutdown();
    renderer.Shutdown();
}

TEST_F(WebGpuTest, InitializeFailuresLeaveNothingBehindAndTheRendererStaysUsable)
{
    WebGpuRenderer& renderer = NewRenderer();
    const std::int64_t liveBefore = WebGpuRenderer::GetLiveObjectCountForTest();

    // Before anything is created
    EXPECT_FALSE(renderer.Initialize(nullptr, 0, 8));
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::Failed);
    EXPECT_FALSE(renderer.Initialize(nullptr, 8, 0));
    EXPECT_EQ(renderer.GetRenderWidth(), 0u);

    // After the instance, adapter and device exist: a target no device can make. The renderer releases all of it.
    EXPECT_FALSE(renderer.Initialize(nullptr, 1u << 30, 4));
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::Failed);
    EXPECT_FALSE(renderer.GetInitMessage().empty());
    EXPECT_EQ(renderer.GetRenderWidth(), 0u);
    EXPECT_TRUE(renderer.GetAdapterName().empty());
    EXPECT_EQ(renderer.GetDeviceForTest(), nullptr);
    EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), liveBefore) << "a failed Initialize leaked references";
    RenderFrame(renderer); // harmless

    EXPECT_FALSE(renderer.Initialize(nullptr, 4, 1u << 30));
    EXPECT_FALSE(renderer.Initialize(nullptr, 0xFFFFFFFFu, 0xFFFFFFFFu));
    EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), liveBefore);

    // And a good Initialize still works on the same object
    ASSERT_TRUE(renderer.Initialize(nullptr, 3, 3)) << renderer.GetInitMessage();
    ClearTo(renderer, {12, 34, 56, 255});
    RenderFrame(renderer);
    EXPECT_TRUE(IsSolid(Read(renderer, 3, 3), 3, 3, {12, 34, 56, 255}));
    EXPECT_EQ(renderer.GetValidationErrorCount(), 0u);
}

TEST_F(WebGpuTest, TenCreateDestroyCyclesLeakNothing)
{
    // Ten renderers, each made, drawn into, read and destroyed: the process holds no WebGPU reference afterwards (the live
    // object count is the renderer's own accounting of every handle it took) and the device reported no error
    const std::int64_t liveBefore = WebGpuRenderer::GetLiveObjectCountForTest();
    for (int cycle = 0; cycle < 10; ++cycle)
    {
        {
            WebGpuRenderer renderer;
            ASSERT_TRUE(renderer.Initialize(nullptr, 64, 48)) << "cycle " << cycle << ": " << renderer.GetInitMessage();
            EXPECT_GT(WebGpuRenderer::GetLiveObjectCountForTest(), liveBefore);
            const Rgba colour{cycle * 20, 255 - cycle * 20, 7, 255};
            ClearTo(renderer, colour);
            RenderFrame(renderer);
            EXPECT_TRUE(IsSolid(Read(renderer, 64, 48), 64, 48, colour)) << "cycle " << cycle;
            ASSERT_TRUE(renderer.SetRenderTargetSize(17, 9));
            renderer.BeginFrame(); // a frame left open at destruction
            EXPECT_EQ(renderer.GetValidationErrorCount(), 0u) << "cycle " << cycle;
        } // destroyed with a frame open
        EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), liveBefore) << "references leaked in cycle " << cycle;
    }
}

TEST_F(WebGpuTest, RepeatedCyclesOfLargeTargetsDoNotGrowTheProcessWithoutBound)
{
    // A coarse check of what the reference count cannot see (memory Dawn or the driver keeps): 2048x2048 targets are 16 MB of
    // colour and 16 MB of depth, and a read-back copies 16 MB more, so a renderer whose targets were never freed would add
    // about 48 MB a cycle. After a warm-up cycle (the driver's own one-time allocations), ten more may add less than 150 MB
    const auto cycle = []
    {
        WebGpuRenderer renderer;
        EXPECT_TRUE(renderer.Initialize(nullptr, 2048, 2048)) << renderer.GetInitMessage();
        ClearTo(renderer, {1, 2, 3, 255});
        RenderFrame(renderer);
        const std::vector<std::uint8_t> pixels = Read(renderer, 2048, 2048);
        EXPECT_EQ(pixels.size(), 2048u * 2048u * 4u);
        EXPECT_EQ(renderer.GetValidationErrorCount(), 0u);
    };
    cycle();
    const std::uint64_t baseline = ResidentBytes();
    if (baseline == 0)
    {
        GTEST_SKIP() << "/proc/self/statm is not readable here";
    }
    for (int i = 0; i < 10; ++i)
    {
        cycle();
    }
    const std::uint64_t after = ResidentBytes();
    const std::uint64_t growth = after > baseline ? after - baseline : 0;
    std::cout << "[ webgpu ] resident set after the warm-up cycle: " << baseline / (1024 * 1024) << " MB, after ten more: "
              << after / (1024 * 1024) << " MB" << std::endl;
    EXPECT_LT(growth, 150ull * 1024 * 1024) << "the process grew by " << growth / (1024 * 1024) << " MB over ten cycles";
}

TEST_F(WebGpuTest, AdapterSelectionFollowsThePreferenceAndTheEnvironment)
{
    // CpuOnly: a CPU adapter or no adapter at all, never a real GPU
    {
        WebGpuRenderer& renderer = NewRenderer();
        renderer.SetAdapterPreference(AdapterPreference::CpuOnly);
        if (renderer.Initialize(nullptr, 2, 2))
        {
            EXPECT_EQ(renderer.GetAdapterType(), WGPUAdapterType_CPU);
        }
        else
        {
            EXPECT_EQ(renderer.GetInitStatus(), InitStatus::NoAdapter) << renderer.GetInitMessage();
            EXPECT_NE(Env("N2_WEBGPU_EXPECT_CPU_DEVICE"), "1") << "CI pins a CPU adapter but CpuOnly found none";
        }
    }
    // Any works wherever the fixture's renderer does
    {
        WebGpuRenderer& renderer = NewRenderer();
        renderer.SetAdapterPreference(AdapterPreference::Any);
        ASSERT_TRUE(renderer.Initialize(nullptr, 2, 2)) << renderer.GetInitMessage();
        EXPECT_FALSE(renderer.GetAdapterName().empty());
    }
    // Default reads N2_WEBGPU_EXPECT_CPU_DEVICE: =1 means a CPU adapter, =0 does not ask for one
    {
        ScopedEnvironment cpu("N2_WEBGPU_EXPECT_CPU_DEVICE", "1");
        WebGpuRenderer& renderer = NewRenderer();
        if (renderer.Initialize(nullptr, 2, 2))
        {
            EXPECT_EQ(renderer.GetAdapterType(), WGPUAdapterType_CPU);
        }
        else
        {
            EXPECT_EQ(renderer.GetInitStatus(), InitStatus::NoAdapter) << renderer.GetInitMessage();
            EXPECT_NE(renderer.GetInitMessage().find("N2_WEBGPU_EXPECT_CPU_DEVICE"), std::string::npos)
                << "the message does not name the variable: " << renderer.GetInitMessage();
        }
    }
    {
        ScopedEnvironment any("N2_WEBGPU_EXPECT_CPU_DEVICE", "0");
        WebGpuRenderer& renderer = NewRenderer();
        ASSERT_TRUE(renderer.Initialize(nullptr, 2, 2)) << renderer.GetInitMessage();
    }
}

TEST_F(WebGpuTest, WithoutAVulkanDriverInitializeFailsWithNoAdapterAndAClearMessage)
{
    // Pointing the Vulkan loader at a driver file that does not exist leaves Dawn no adapter: the machine-lacks-WebGPU path
    // that tests skip on (or fail on under N2_REQUIRE_WEBGPU=1). If the loader ignores the change (it read the variable once),
    // this machine cannot show the path and the test says so
    ScopedEnvironment drivers("VK_DRIVER_FILES", "/nonexistent/n2-no-such-icd.json");
    ScopedEnvironment legacyDrivers("VK_ICD_FILENAMES", "/nonexistent/n2-no-such-icd.json");
    WebGpuRenderer& renderer = NewRenderer();
    if (renderer.Initialize(nullptr, 2, 2))
    {
        GTEST_SKIP() << "the Vulkan loader still found a driver after VK_DRIVER_FILES changed";
    }
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::NoAdapter) << renderer.GetInitMessage();
    EXPECT_NE(renderer.GetInitMessage().find("adapter"), std::string::npos) << renderer.GetInitMessage();
    EXPECT_TRUE(EnvironmentLacksWebGpu(renderer.GetInitStatus()));
    EXPECT_EQ(renderer.GetDeviceForTest(), nullptr);
    EXPECT_EQ(renderer.GetRenderWidth(), 0u);
}
