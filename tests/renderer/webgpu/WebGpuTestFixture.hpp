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

#include <renderer/webgpu/WebGpuRenderer.hpp>

#include "common/BlendReference.hpp"
#include "common/CrossBackendDiff.hpp"

// The fixture and helpers of the WebGPU backend's tests (stage G2 of #155). Every test runs a WebGpuRenderer on Dawn's Vulkan
// backend, which in CI is Mesa lavapipe, and the fixture fails the test if the device reported ANY error (an uncaptured
// validation error, an out-of-memory, or a device loss the renderer did not cause: WebGpuRenderer::GetValidationErrorCount) or
// if a WebGPU object is still referenced after every renderer of the test was shut down
// (WebGpuRenderer::GetLiveObjectCountForTest). Where there is no WebGPU adapter the tests GTEST_SKIP, unless
// N2_REQUIRE_WEBGPU=1 (set in the CI job) makes that a failure.
//
// Environment:
//   N2_REQUIRE_WEBGPU=1            no instance / no adapter is a failure, not a skip
//   N2_WEBGPU_EXPECT_CPU_DEVICE=1  the renderer takes only a CPU adapter (as lavapipe is), and the fixture asserts it got one:
//                                  proves the CI pinned it. The renderer itself reads this variable (adapter selection)

namespace WebGpuTesting
{
    // The cross-backend helpers of tests/renderer/common (the diff and the blend reference) were made for the Vulkan tests and
    // keep the namespace they were made in; the WebGPU tests reach them as Diff:: and Blend::
    namespace Diff = ::VulkanTesting::Diff;
    namespace Blend = ::VulkanTesting::Blend;

    using Renderer::WebGpu::AdapterPreference;
    using Renderer::WebGpu::InitStatus;
    using Renderer::WebGpu::WebGpuRenderer;

    inline std::string Env(const char* name)
    {
        std::string value;
        if (const char* text = std::getenv(name); text != nullptr)
        {
            value = text;
        }
        return value;
    }

    inline bool EnvironmentLacksWebGpu(InitStatus status)
    {
        return status == InitStatus::NoInstance || status == InitStatus::NoAdapter;
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

    inline void ClearTo(WebGpuRenderer& renderer, const Rgba& c)
    {
        renderer.Clear(Unorm(c.r), Unorm(c.g), Unorm(c.b), Unorm(c.a));
    }

    inline void RenderFrame(WebGpuRenderer& renderer)
    {
        renderer.BeginFrame();
        renderer.EndFrame();
        renderer.Present();
    }

    inline std::vector<std::uint8_t> Read(const WebGpuRenderer& renderer, int width, int height)
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

    /// What the fixture's teardown reports for a renderer: empty when the device reported no error, else the count and the
    /// first messages. A test of the counter itself uses it to prove the report is not empty after an error
    inline std::string ValidationReport(const WebGpuRenderer& renderer)
    {
        const std::uint32_t count = renderer.GetValidationErrorCount();
        if (count == 0)
        {
            return {};
        }
        std::ostringstream report;
        report << count << " WebGPU error(s):";
        for (const auto& message : renderer.GetValidationMessages())
        {
            report << "\n  " << message;
        }
        return report.str();
    }

    class WebGpuTest : public ::testing::Test
    {
    protected:
        /// A renderer checked for errors at teardown; not initialized
        WebGpuRenderer& NewRenderer()
        {
            _renderers.push_back(std::make_unique<WebGpuRenderer>());
            return *_renderers.back();
        }

        /// What a failed Initialize means for the test: a machine without a WebGPU adapter skips (fails under
        /// N2_REQUIRE_WEBGPU=1), anything else fails
        void HandleInitFailure(const WebGpuRenderer& renderer)
        {
            std::ostringstream reason;
            reason << "Initialize failed (status " << static_cast<int>(renderer.GetInitStatus())
                   << "): " << renderer.GetInitMessage();
            if (EnvironmentLacksWebGpu(renderer.GetInitStatus()))
            {
                if (Env("N2_REQUIRE_WEBGPU") == "1")
                {
                    FAIL() << "N2_REQUIRE_WEBGPU=1 but there is no usable WebGPU adapter. " << reason.str();
                }
                GTEST_SKIP() << "No WebGPU adapter here. " << reason.str();
            }
            FAIL() << reason.str();
        }

        void SetUp() override
        {
            _liveObjectsAtStart = WebGpuRenderer::GetLiveObjectCountForTest();
            _primary = &NewRenderer();
            if (!_primary->Initialize(nullptr, 8, 8))
            {
                HandleInitFailure(*_primary);
                return;
            }
            std::cout << "[ webgpu ] adapter: " << _primary->GetAdapterName() << " (type "
                      << static_cast<int>(_primary->GetAdapterType()) << ", backend "
                      << static_cast<int>(_primary->GetBackendType()) << ")" << std::endl;
            ASSERT_EQ(_primary->GetBackendType(), WGPUBackendType_Vulkan);
            if (Env("N2_WEBGPU_EXPECT_CPU_DEVICE") == "1")
            {
                ASSERT_EQ(_primary->GetAdapterType(), WGPUAdapterType_CPU)
                    << "the adapter is " << _primary->GetAdapterName() << ", not a CPU adapter (lavapipe)";
            }
        }

        void TearDown() override
        {
            for (auto& renderer : _renderers)
            {
                renderer->Shutdown(); // errors still in flight are delivered here
                std::ostringstream messages;
                for (const auto& message : renderer->GetValidationMessages())
                {
                    messages << "\n  " << message;
                }
                EXPECT_EQ(renderer->GetValidationErrorCount(), 0u) << "WebGPU errors:" << messages.str();
            }
            _renderers.clear();
            EXPECT_EQ(WebGpuRenderer::GetLiveObjectCountForTest(), _liveObjectsAtStart)
                << "WebGPU objects are still referenced after every renderer of the test was destroyed (a leak)";
        }

        /// The renderer SetUp initialized at 8x8
        WebGpuRenderer& R() { return *_primary; }

    private:
        std::vector<std::unique_ptr<WebGpuRenderer>> _renderers;
        WebGpuRenderer* _primary = nullptr;
        std::int64_t _liveObjectsAtStart = 0;
    };
}
