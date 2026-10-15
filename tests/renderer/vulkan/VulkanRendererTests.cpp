#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <renderer/common/RendererConfig.hpp>
#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanTestFixture.hpp"

// The Vulkan backend's core (#43, stage 0), run headless: Initialize(nullptr, w, h), a frame that clears the offscreen
// target, ReadFramebuffer, SetRenderTargetSize, Shutdown. They need a Vulkan device and the Khronos validation layer;
// where there is none they GTEST_SKIP, unless N2_REQUIRE_VULKAN=1 (set in the Linux CI jobs, where Mesa lavapipe is
// installed) makes a missing device a failure, so the required check cannot pass by skipping everything.
//
// Every test runs with validation ON (ValidationMode::Enabled demands the layer), and the fixture fails the test if the
// layer reported any error (a leaked Vulkan object at device destroy is one) or any validation warning, for the fixture's
// renderer and for every renderer the test made through NewRenderer().
//
// Environment:
//   N2_REQUIRE_VULKAN=1            no device / no validation layer is a failure, not a skip
//   N2_VULKAN_EXPECT_CPU_DEVICE=1  the device must be a CPU (software) device, as lavapipe is: proves the CI pinned it
//   N2_VULKAN_VALIDATION is ignored here (the tests set ValidationMode::Enabled themselves)

using Renderer::Vulkan::InitStatus;
using Renderer::Vulkan::ValidationMode;
using Renderer::Vulkan::VulkanRenderer;
using namespace VulkanTesting; // the fixture and helpers

namespace
{
    struct Size
    {
        int w, h;
    };

    class VulkanSizeTest : public VulkanTest, public ::testing::WithParamInterface<Size>
    {
    };
}

TEST_F(VulkanTest, InitializesHeadlessWithAnOkStatusAndADevice)
{
    EXPECT_EQ(R().GetInitStatus(), InitStatus::Ok);
    EXPECT_TRUE(R().GetInitMessage().empty());
    EXPECT_FALSE(R().GetDeviceName().empty());
    EXPECT_EQ(R().GetRenderWidth(), 8u);
    EXPECT_EQ(R().GetRenderHeight(), 8u);
    EXPECT_STREQ(R().GetRendererName(), "Vulkan Renderer");
}

TEST_F(VulkanTest, ValidationCanaryTheDebugMessengerReallyCountsErrorsAndKeepsTheirText)
{
    // Proves the zero-errors assertion of every other test can fail: a synthetic message goes through the SAME debug
    // messenger the validation layer reports through, and must be counted and kept
    ASSERT_EQ(R().GetValidationStats().errors, 0u);
    ASSERT_EQ(R().GetValidationStats().warnings, 0u);

    ASSERT_TRUE(R().SubmitDebugMessageForTest(true, "n2-canary-error-message"));
    const auto afterError = R().GetValidationStats();
    EXPECT_EQ(afterError.errors, 1u);
    EXPECT_EQ(afterError.warnings, 0u);
    bool errorKept = false;
    for (const auto& message : R().GetValidationMessages())
    {
        errorKept = errorKept || message.find("n2-canary-error-message") != std::string::npos;
    }
    EXPECT_TRUE(errorKept) << "the error text was not retained";

    ASSERT_TRUE(R().SubmitDebugMessageForTest(false, "n2-canary-warning-message"));
    const auto afterWarning = R().GetValidationStats();
    EXPECT_EQ(afterWarning.errors, 1u);
    EXPECT_EQ(afterWarning.warnings, 1u);

    // The counters reset, so this test's own TearDown (zero errors, zero warnings) passes
    R().ResetValidationStats();
    EXPECT_EQ(R().GetValidationStats().errors, 0u);
    EXPECT_EQ(R().GetValidationStats().warnings, 0u);
    EXPECT_TRUE(R().GetValidationMessages().empty());
}

TEST_F(VulkanTest, PresentWarnsOncePerRendererThatThereIsNoSwapchainYet)
{
    VulkanRenderer& renderer = NewRenderer();
    ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();

    const auto countWarnings = [](const std::string& output)
    {
        const std::string needle = "swapchain/present is not implemented yet (stage F)";
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

TEST_P(VulkanSizeTest, ClearColourReadsBackExactlyWhenInitializedAtTheSize)
{
    const Size size = GetParam();
    VulkanRenderer& renderer = NewRenderer();
    ASSERT_TRUE(renderer.Initialize(nullptr, static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)))
        << renderer.GetInitMessage();
    EXPECT_EQ(renderer.GetRenderWidth(), static_cast<uint32_t>(size.w));
    EXPECT_EQ(renderer.GetRenderHeight(), static_cast<uint32_t>(size.h));

    const Rgba colour{10, 200, 30, 255};
    ClearTo(renderer, colour);
    RenderFrame(renderer);
    EXPECT_TRUE(IsSolid(Read(renderer, size.w, size.h), size.w, size.h, colour));
}

TEST_P(VulkanSizeTest, ClearColourReadsBackExactlyAfterSetRenderTargetSize)
{
    const Size size = GetParam();
    ASSERT_TRUE(R().SetRenderTargetSize(static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)));

    const Rgba colour{255, 1, 128, 77};
    ClearTo(R(), colour);
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, colour));
}

INSTANTIATE_TEST_SUITE_P(Sizes, VulkanSizeTest,
                         ::testing::Values(Size{1, 1}, Size{7, 5}, Size{64, 64}, Size{3, 1}, Size{1, 9}, Size{33, 17},
                                           Size{256, 2}, Size{200, 120}));

TEST_F(VulkanTest, ClearColourChangesBetweenFrames)
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

TEST_F(VulkanTest, ManyFramesWithoutReadbackCycleTheFramesInFlight)
{
    // More frames than frames in flight (2), each with its own colour and none read back: the last one wins
    Rgba last{};
    for (int i = 0; i < 9; ++i)
    {
        last = {i * 20, 255 - i * 20, i * 3, 255};
        ClearTo(R(), last);
        RenderFrame(R());
    }
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, last));
}

TEST_F(VulkanTest, AClearInTheMiddleOfAFrameReplacesTheFrame)
{
    ClearTo(R(), {255, 0, 0, 255});
    R().BeginFrame();
    ClearTo(R(), {0, 0, 255, 255});
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {0, 0, 255, 255}));
}

TEST_F(VulkanTest, TheFrameBeforeTheFirstOneReadsBlack)
{
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, Black));
}

TEST_F(VulkanTest, SetRenderTargetSizeThenReadbackAtEachSize)
{
    const Size sizes[] = {{13, 3}, {1, 1}, {64, 64}, {5, 40}, {8, 8}};
    for (const Size& size : sizes)
    {
        ASSERT_TRUE(R().SetRenderTargetSize(static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)));
        EXPECT_EQ(R().GetRenderWidth(), static_cast<uint32_t>(size.w));
        EXPECT_EQ(R().GetRenderHeight(), static_cast<uint32_t>(size.h));
        // A new target starts black, then a frame fills it
        EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, Black));
        const Rgba colour{size.w * 3 % 256, size.h * 5 % 256, 40, 255};
        ClearTo(R(), colour);
        RenderFrame(R());
        EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, colour));
    }
}

TEST_F(VulkanTest, SetRenderTargetSizeRejectsAZeroDimensionAndKeepsTheTarget)
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

TEST_F(VulkanTest, SetRenderTargetSizeOverTheDeviceLimitFailsAndALaterCallRecovers)
{
    EXPECT_FALSE(R().SetRenderTargetSize(1u << 30, 4));
    EXPECT_EQ(R().GetRenderWidth(), 0u); // the old target is gone
    ClearTo(R(), {1, 2, 3, 4});
    RenderFrame(R()); // skipped: no target
    EXPECT_TRUE(R().SetRenderTargetSize(6, 4));
    ClearTo(R(), {1, 2, 3, 4});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 6, 4), 6, 4, {1, 2, 3, 4}));
}

TEST_F(VulkanTest, ResizeFollowsUntilSetRenderTargetSizeFixesTheSize)
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

TEST_F(VulkanTest, ReadingAtAnotherSizeResamplesNearest)
{
    const Rgba colour{70, 80, 90, 255};
    ClearTo(R(), colour);
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 3, 2), 3, 2, colour));
    EXPECT_TRUE(IsSolid(Read(R(), 16, 16), 16, 16, colour));
    EXPECT_TRUE(IsSolid(Read(R(), 1, 1), 1, 1, colour));
}

TEST_F(VulkanTest, ReadFramebufferIgnoresBadArguments)
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

TEST_F(VulkanTest, ReinitializeAfterShutdown)
{
    const std::weak_ptr<const void> first = R().GetLifetimeToken();
    EXPECT_FALSE(first.expired());
    R().Shutdown();
    EXPECT_TRUE(first.expired());
    EXPECT_EQ(R().GetRenderWidth(), 0u);

    for (const Size size : {Size{5, 3}, Size{16, 16}, Size{1, 2}})
    {
        ASSERT_TRUE(R().Initialize(nullptr, static_cast<uint32_t>(size.w), static_cast<uint32_t>(size.h)))
            << R().GetInitMessage();
        EXPECT_EQ(R().GetInitStatus(), InitStatus::Ok);
        const std::weak_ptr<const void> token = R().GetLifetimeToken();
        EXPECT_FALSE(token.expired());
        EXPECT_TRUE(first.expired()); // a new renderer, not the old one back

        const Rgba colour{size.w * 7 % 256, 100, size.h * 11 % 256, 255};
        ClearTo(R(), colour);
        RenderFrame(R());
        EXPECT_TRUE(IsSolid(Read(R(), size.w, size.h), size.w, size.h, colour));

        R().Shutdown();
        EXPECT_TRUE(token.expired());
    }
}

TEST_F(VulkanTest, InitializeAgainWithoutShutdownReplacesTheRenderer)
{
    const std::weak_ptr<const void> token = R().GetLifetimeToken();
    ASSERT_TRUE(R().Initialize(nullptr, 4, 4)) << R().GetInitMessage();
    EXPECT_TRUE(token.expired());
    ClearTo(R(), {200, 100, 50, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 4, 4), 4, 4, {200, 100, 50, 255}));
}

TEST_F(VulkanTest, ShutdownTwiceIsSafeAndEverythingAfterItIsHarmless)
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
}

TEST_F(VulkanTest, ARendererThatWasNeverInitializedIsHarmless)
{
    VulkanRenderer& renderer = NewRenderer();
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::NotInitialized);
    RenderFrame(renderer);
    EXPECT_FALSE(renderer.SetRenderTargetSize(4, 4));
    std::vector<std::uint8_t> pixels(16, 0xAB);
    renderer.ReadFramebuffer(pixels.data(), 2, 2);
    EXPECT_EQ(pixels[0], 0xAB);
    renderer.Shutdown();
    renderer.Shutdown();
}

TEST_F(VulkanTest, InitializeFailuresLeaveNothingBehindAndTheRendererStaysUsable)
{
    VulkanRenderer& renderer = NewRenderer();

    // Before anything is created
    EXPECT_FALSE(renderer.Initialize(nullptr, 0, 8));
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::Failed);
    EXPECT_FALSE(renderer.Initialize(nullptr, 8, 0));
    EXPECT_EQ(renderer.GetRenderWidth(), 0u);

    // After the instance and device exist: a target no device can make. The renderer destroys the device and instance.
    EXPECT_FALSE(renderer.Initialize(nullptr, 1u << 30, 4));
    EXPECT_EQ(renderer.GetInitStatus(), InitStatus::Failed);
    EXPECT_FALSE(renderer.GetInitMessage().empty());
    EXPECT_EQ(renderer.GetRenderWidth(), 0u);
    EXPECT_FALSE(renderer.IsValidationEnabled());
    EXPECT_TRUE(renderer.GetDeviceName().empty());
    RenderFrame(renderer); // harmless

    EXPECT_FALSE(renderer.Initialize(nullptr, 4, 1u << 30));
    EXPECT_FALSE(renderer.Initialize(nullptr, 0xFFFFFFFFu, 0xFFFFFFFFu));

    // And a good Initialize still works on the same object
    ASSERT_TRUE(renderer.Initialize(nullptr, 3, 3)) << renderer.GetInitMessage();
    ClearTo(renderer, {12, 34, 56, 255});
    RenderFrame(renderer);
    EXPECT_TRUE(IsSolid(Read(renderer, 3, 3), 3, 3, {12, 34, 56, 255}));
}

TEST_F(VulkanTest, ValidationCanBeLeftOff)
{
    VulkanRenderer renderer;
    renderer.SetValidationMode(ValidationMode::Disabled);
    ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();
    EXPECT_FALSE(renderer.IsValidationEnabled());
    ClearTo(renderer, {1, 2, 3, 255});
    RenderFrame(renderer);
    EXPECT_TRUE(IsSolid(Read(renderer, 4, 4), 4, 4, {1, 2, 3, 255}));
    const auto stats = renderer.GetValidationStats();
    EXPECT_EQ(stats.errors, 0u);
    EXPECT_EQ(stats.warnings, 0u);
}

TEST_F(VulkanTest, FrameCallsOutOfOrderAreHarmless)
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
}

TEST_F(VulkanTest, EverythingThatWouldDrawFailsCleanly)
{
    // Stages B-E of #43 are not here yet: materials, shaders and draws log once and return null/false (or draw
    // nothing), and a frame still renders. Meshes and textures (stage A) work: see VulkanResourceTests.cpp
    Renderer::Common::MeshData mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    Renderer::Common::IMesh* const liveMesh = R().CreateMesh(mesh);
    ASSERT_NE(liveMesh, nullptr);
    EXPECT_TRUE(R().UpdateMesh(liveMesh, mesh));
    EXPECT_FALSE(R().UpdateMesh(nullptr, mesh));
    const std::uint8_t texel[4] = {1, 2, 3, 4};
    Renderer::Common::ITexture* const plainTexture = R().CreateTexture(texel, 1, 1, 4);
    Renderer::Common::ITexture* const optionTexture =
        R().CreateTexture(texel, 1, 1, 4, Renderer::Common::TextureOptions{});
    EXPECT_NE(plainTexture, nullptr);
    EXPECT_NE(optionTexture, nullptr);
    EXPECT_EQ(R().CreateMaterial(nullptr), nullptr);
    EXPECT_EQ(R().CreateMaterial(nullptr, nullptr), nullptr);
    EXPECT_EQ(R().CreateShaderProgram("void main(){}", "void main(){}"), nullptr);
    EXPECT_EQ(R().GetStandardUnlitShader(), nullptr);
    EXPECT_EQ(R().GetStandardLitShader(), nullptr);
    EXPECT_EQ(R().GetStandardTextShader(), nullptr);
    EXPECT_FALSE(R().IsValidShader(nullptr));
    EXPECT_FALSE(R().DestroyShaderProgram(nullptr));
    R().UseShaderProgram(nullptr);
    R().DestroyMesh(nullptr);
    R().DestroyTexture(nullptr);
    R().DestroyMaterial(nullptr);
    EXPECT_TRUE(R().GetCompatibleGPUs().empty());

    constexpr float Identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    R().BeginFrame();
    R().SetViewProjection(Identity, Identity);
    R().UpdateSceneLighting(Renderer::Common::SceneLightingData{}, N2Engine::Math::Vector3{});
    R().SetWireframe(true);
    R().DrawMesh(nullptr, Identity, nullptr);
    R().DrawMesh(nullptr, Identity, nullptr, Renderer::Common::RenderState{}, Renderer::Common::IndexRange{0, 3});
    R().DrawMesh(liveMesh, Identity, nullptr); // a real mesh, still nothing to draw it with
    R().DrawMesh(liveMesh, Identity, nullptr, Renderer::Common::RenderState{}, Renderer::Common::IndexRange{0, 3});
    std::vector<Renderer::Common::RenderObject> objects(1);
    R().DrawObjects(objects);
    R().SetWireframe(false);
    ClearTo(R(), {44, 55, 66, 255});
    R().EndFrame();
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {44, 55, 66, 255}));
    R().DestroyMesh(liveMesh);
    R().DestroyTexture(plainTexture);
    R().DestroyTexture(optionTexture);
}
