#pragma once

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

// The fixture and helpers of the Vulkan backend's tests (moved here unchanged from VulkanRendererTests.cpp so the
// resource tests share them). Every test runs with validation ON (ValidationMode::Enabled demands the layer), and the
// fixture fails the test if the layer reported any error (a leaked Vulkan object at device destroy is one) or any
// validation warning, for the fixture's renderer and for every renderer the test made through NewRenderer(). Where
// there is no Vulkan device the tests GTEST_SKIP, unless N2_REQUIRE_VULKAN=1 (set in the Linux CI jobs) makes that a
// failure.
//
// Environment:
//   N2_REQUIRE_VULKAN=1            no device / no validation layer is a failure, not a skip
//   N2_VULKAN_EXPECT_CPU_DEVICE=1  the device must be a CPU (software) device, as lavapipe is: proves the CI pinned it
//   N2_VULKAN_VALIDATION is ignored here (the tests set ValidationMode::Enabled themselves)
//
// The draw tests (stage B of #43) also need the built-in shaders, which the renderer's CMake compiles to SPIR-V when it
// is built (glslangValidator or glslc). A build that found neither has none: N2_REQUIRE_BUILT_IN_SHADERS() skips a draw
// test there, with the reason, unless N2_REQUIRE_VULKAN=1 (the Linux CI legs), where it is a failure, so those legs can
// never pass by silently skipping every draw.

namespace VulkanTesting
{
    using Renderer::Vulkan::InitStatus;
    using Renderer::Vulkan::ValidationMode;
    using Renderer::Vulkan::VulkanRenderer;

    inline std::string Env(const char* name)
    {
        std::string value;
#ifdef _MSC_VER
        char* buffer = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&buffer, &length, name) == 0 && buffer != nullptr)
        {
            value = buffer;
            std::free(buffer);
        }
#else
        if (const char* text = std::getenv(name); text != nullptr)
        {
            value = text;
        }
#endif
        return value;
    }

    inline bool EnvironmentLacksVulkan(InitStatus status)
    {
        return status == InitStatus::NoVulkanRuntime || status == InitStatus::NoSuitableDevice ||
            status == InitStatus::ValidationUnavailable;
    }

    struct Rgba
    {
        int r, g, b, a;
    };

    /// A clear colour component that is exactly k/255, so the 8-bit target holds exactly k
    inline float Unorm(int k)
    {
        return static_cast<float>(k) / 255.0f;
    }

    inline void ClearTo(VulkanRenderer& renderer, const Rgba& c)
    {
        renderer.Clear(Unorm(c.r), Unorm(c.g), Unorm(c.b), Unorm(c.a));
    }

    inline void RenderFrame(VulkanRenderer& renderer)
    {
        renderer.BeginFrame();
        renderer.EndFrame();
        renderer.Present();
    }

    inline std::vector<std::uint8_t> Read(const VulkanRenderer& renderer, int width, int height)
    {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0xCD);
        renderer.ReadFramebuffer(pixels.data(), width, height);
        return pixels;
    }

    /// Every pixel is exactly `expected`
    inline ::testing::AssertionResult IsSolid(const std::vector<std::uint8_t>& pixels, int width, int height,
                                       const Rgba& expected)
    {
        if (pixels.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4)
        {
            return ::testing::AssertionFailure() << "the buffer is not width * height * 4 bytes";
        }
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::uint8_t* p = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
                if (p[0] != expected.r || p[1] != expected.g || p[2] != expected.b || p[3] != expected.a)
                {
                    return ::testing::AssertionFailure()
                        << "pixel (" << x << "," << y << ") of " << width << "x" << height << " is (" << int(p[0])
                        << "," << int(p[1]) << "," << int(p[2]) << "," << int(p[3]) << "), expected (" << expected.r
                        << "," << expected.g << "," << expected.b << "," << expected.a << ")";
                }
            }
        }
        return ::testing::AssertionSuccess();
    }

    inline const Rgba Black{0, 0, 0, 0};

    /// Why a test that needs the built-in shaders cannot run here, or empty when it can
    inline std::string MissingShadersReason()
    {
        return VulkanRenderer::HasBuiltInShaders()
            ? std::string()
            : std::string("this build has no compiled Vulkan shaders (neither glslangValidator nor glslc was found when "
                          "CMake configured the renderer)");
    }

    class VulkanTest : public ::testing::Test
    {
    protected:
        /// A renderer with validation demanded, checked for validation messages at teardown; not initialized
        VulkanRenderer& NewRenderer()
        {
            auto renderer = std::make_unique<VulkanRenderer>();
            renderer->SetValidationMode(ValidationMode::Enabled);
            _renderers.push_back(std::move(renderer));
            return *_renderers.back();
        }

        /// What a failed Initialize means for the test: a machine without Vulkan skips (fails under
        /// N2_REQUIRE_VULKAN=1), anything else fails
        void HandleInitFailure(const VulkanRenderer& renderer)
        {
            std::ostringstream reason;
            reason << "Initialize failed (status " << static_cast<int>(renderer.GetInitStatus())
                   << "): " << renderer.GetInitMessage();
            if (EnvironmentLacksVulkan(renderer.GetInitStatus()))
            {
                if (Env("N2_REQUIRE_VULKAN") == "1")
                {
                    FAIL() << "N2_REQUIRE_VULKAN=1 but there is no usable Vulkan device or validation layer. "
                           << reason.str();
                }
                GTEST_SKIP() << "No Vulkan device or validation layer here. " << reason.str();
            }
            FAIL() << reason.str();
        }

        void SetUp() override
        {
            _primary = &NewRenderer();
            if (!_primary->Initialize(nullptr, 8, 8))
            {
                HandleInitFailure(*_primary);
                return;
            }
            std::cout << "[ vulkan ] device: " << _primary->GetDeviceName()
                      << (_primary->IsValidationEnabled() ? " (validation on)" : " (validation OFF)") << std::endl;
            ASSERT_TRUE(_primary->IsValidationEnabled());
            if (Env("N2_VULKAN_EXPECT_CPU_DEVICE") == "1")
            {
                ASSERT_EQ(_primary->GetDeviceType(), VK_PHYSICAL_DEVICE_TYPE_CPU)
                    << "the device is " << _primary->GetDeviceName() << ", not a CPU device (lavapipe)";
            }
        }

        void TearDown() override
        {
            for (auto& renderer : _renderers)
            {
                renderer->Shutdown(); // the layer reports every undestroyed object here
                const auto stats = renderer->GetValidationStats();
                std::ostringstream messages;
                for (const auto& message : renderer->GetValidationMessages())
                {
                    messages << "\n  " << message;
                }
                EXPECT_EQ(stats.errors, 0u) << "validation errors:" << messages.str();
                EXPECT_EQ(stats.warnings, 0u) << "validation warnings:" << messages.str();
            }
            _renderers.clear();
        }

        /// The renderer SetUp initialized at 8x8
        VulkanRenderer& R() { return *_primary; }

    private:
        std::vector<std::unique_ptr<VulkanRenderer>> _renderers;
        VulkanRenderer* _primary = nullptr;
    };
}

/// Put at the top of a test (or a fixture's SetUp) that draws: skips it where the build has no compiled shaders, fails it
/// under N2_REQUIRE_VULKAN=1
#define N2_REQUIRE_BUILT_IN_SHADERS()                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!::Renderer::Vulkan::VulkanRenderer::HasBuiltInShaders())                                                  \
        {                                                                                                              \
            if (::VulkanTesting::Env("N2_REQUIRE_VULKAN") == "1")                                                      \
            {                                                                                                          \
                FAIL() << "N2_REQUIRE_VULKAN=1 but " << ::VulkanTesting::MissingShadersReason();                       \
            }                                                                                                          \
            GTEST_SKIP() << ::VulkanTesting::MissingShadersReason();                                                   \
        }                                                                                                              \
    } while (false)

