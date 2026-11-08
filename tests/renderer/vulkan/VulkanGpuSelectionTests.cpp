#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <renderer/vulkan/VulkanRenderer.hpp>

#include "VulkanTestFixture.hpp"

// The GPU selection API of the Vulkan backend (stage F of #43) on a real instance: on the CI legs that is Mesa lavapipe,
// the only device (pinned through VK_DRIVER_FILES), so these prove the API's plumbing and that lavapipe is still chosen;
// the choice between several devices is the pure function's (VulkanDeviceChoiceTests.cpp) and the GPU checklist's
// (docs/gpu-vulkan-checklist.html, two GPUs). Validation is on and every test fails on a validation message or a leak, as
// everywhere in tests/renderer/vulkan/.

using Renderer::Vulkan::GPUInfo;
using Renderer::Vulkan::VulkanRenderer;
using namespace VulkanTesting;

namespace
{
    void SetEnvironment(const char* name, const std::string& value)
    {
#ifdef _MSC_VER
        _putenv_s(name, value.c_str()); // an empty value removes the variable
#else
        if (value.empty())
        {
            unsetenv(name);
        }
        else
        {
            setenv(name, value.c_str(), 1);
        }
#endif
    }

    /// Sets an environment variable for the scope, and puts the old value back
    class ScopedEnvironment
    {
    public:
        ScopedEnvironment(const char* name, const std::string& value) : _name(name), _old(Env(name))
        {
            SetEnvironment(_name, value);
        }
        ~ScopedEnvironment() { SetEnvironment(_name, _old); }
        ScopedEnvironment(const ScopedEnvironment&) = delete;
        ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    private:
        const char* _name;
        std::string _old;
    };

    int Count(const std::string& text, const std::string& needle)
    {
        int count = 0;
        for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1))
        {
            ++count;
        }
        return count;
    }

    std::string Lowercase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    /// The position in GetCompatibleGPUs (= the enumeration index) of the device the renderer uses, or -1
    int IndexInUse(VulkanRenderer& renderer, const std::vector<GPUInfo>& gpus)
    {
        for (std::size_t i = 0; i < gpus.size(); ++i)
        {
            if (renderer.SelectGPU(gpus[i].device))
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    GPUInfo FakeGpu(std::uintptr_t handle, const char* name, VkPhysicalDeviceType type, std::uint64_t vramMB, bool supported)
    {
        GPUInfo gpu;
        gpu.device = reinterpret_cast<VkPhysicalDevice>(handle);
        gpu.name = name;
        gpu.type = type;
        gpu.vramMB = vramMB;
        gpu.isSupported = supported;
        return gpu;
    }

    constexpr const char* Nonsense = "no-such-gpu-n2engine-test";
}

TEST_F(VulkanTest, GetCompatibleGPUsListsTheDeviceInUseAsSupportedAndTheDefaultRecommendsIt)
{
    const std::vector<GPUInfo> gpus = R().GetCompatibleGPUs();
    ASSERT_FALSE(gpus.empty());
    for (std::size_t i = 0; i < gpus.size(); ++i)
    {
        std::cout << "[ vulkan ] GPU " << i << ": " << gpus[i].name << ", type " << gpus[i].type << ", "
                  << gpus[i].vramMB << " MiB, " << (gpus[i].isSupported ? "supported" : "not supported") << std::endl;
        EXPECT_NE(gpus[i].device, VK_NULL_HANDLE);
        EXPECT_FALSE(gpus[i].name.empty());
    }

    const int inUse = IndexInUse(R(), gpus);
    ASSERT_GE(inUse, 0) << "SelectGPU accepts none of the listed devices";
    const GPUInfo& current = gpus[static_cast<std::size_t>(inUse)];
    EXPECT_TRUE(current.isSupported);
    EXPECT_EQ(current.name, R().GetDeviceName());
    EXPECT_EQ(current.type, R().GetDeviceType());
    EXPECT_GE(current.apiVersion, VK_API_VERSION_1_1);
    int accepted = 0;
    for (const GPUInfo& gpu : gpus)
    {
        accepted += R().SelectGPU(gpu.device) ? 1 : 0;
    }
    EXPECT_EQ(accepted, 1) << "SelectGPU is true only for the device in use";

    // With no preference (the CI legs set none) the headless choice is the default rule's, as before stage F
    if (Env("N2_VULKAN_GPU").empty())
    {
        EXPECT_EQ(R().GetRecommendedGPU(gpus), current.device);
    }
    if (Env("N2_VULKAN_EXPECT_CPU_DEVICE") == "1")
    {
        EXPECT_EQ(current.type, VK_PHYSICAL_DEVICE_TYPE_CPU) << "lavapipe must still be chosen";
    }
}

TEST_F(VulkanTest, GpuSelectionBeforeInitializeAndAfterShutdown)
{
    VulkanRenderer& renderer = NewRenderer();
    EXPECT_TRUE(renderer.GetCompatibleGPUs().empty()) << "no instance yet";
    const std::vector<GPUInfo> fromPrimary = R().GetCompatibleGPUs();
    ASSERT_FALSE(fromPrimary.empty());
    EXPECT_FALSE(renderer.SelectGPU(fromPrimary[0].device)) << "SelectGPU before Initialize";

    ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();
    const std::vector<GPUInfo> gpus = renderer.GetCompatibleGPUs();
    ASSERT_EQ(gpus.size(), fromPrimary.size());
    EXPECT_GE(IndexInUse(renderer, gpus), 0);
    EXPECT_FALSE(renderer.SelectGPU(VK_NULL_HANDLE));

    renderer.Shutdown();
    EXPECT_TRUE(renderer.GetCompatibleGPUs().empty()) << "after Shutdown";
    EXPECT_FALSE(renderer.SelectGPU(gpus[0].device));
}

TEST_F(VulkanTest, SelectGPUOfAnotherDeviceIsRefusedWithOneLogLine)
{
    const VkPhysicalDevice notInUse = reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{0x1234});
    ::testing::internal::CaptureStderr();
    EXPECT_FALSE(R().SelectGPU(notInUse));
    const std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(Count(output, "SelectGPU cannot switch the device of a running renderer"), 1) << output;
    EXPECT_NE(output.find("SetGpuPreference"), std::string::npos) << output;

    // The renderer is unchanged and still draws its frames
    ClearTo(R(), {33, 66, 99, 255});
    RenderFrame(R());
    EXPECT_TRUE(IsSolid(Read(R(), 8, 8), 8, 8, {33, 66, 99, 255}));
}

TEST_F(VulkanTest, GetRecommendedGPUAppliesTheDefaultRuleToTheListItIsGiven)
{
    const std::vector<GPUInfo> gpus = {
        FakeGpu(0x10, "unsupported discrete", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, 12000, false),
        FakeGpu(0x20, "integrated", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 1000, true),
        FakeGpu(0x30, "cpu", VK_PHYSICAL_DEVICE_TYPE_CPU, 0, true),
        FakeGpu(0x40, "integrated with more memory", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, 2000, true),
    };
    EXPECT_EQ(R().GetRecommendedGPU(gpus), gpus[3].device) << "a supported integrated GPU, the one with more memory";
    EXPECT_EQ(R().GetRecommendedGPU({gpus[0], gpus[2]}), gpus[2].device) << "an unsupported device is never recommended";
    EXPECT_EQ(R().GetRecommendedGPU({gpus[0]}), VK_NULL_HANDLE);
    EXPECT_EQ(R().GetRecommendedGPU({}), VK_NULL_HANDLE);
}

TEST_F(VulkanTest, SetGpuPreferenceByNameOrIndexChoosesThatDevice)
{
    const std::vector<GPUInfo> gpus = R().GetCompatibleGPUs();
    const int index = IndexInUse(R(), gpus);
    ASSERT_GE(index, 0);
    const std::string name = gpus[static_cast<std::size_t>(index)].name;

    VulkanRenderer& byName = NewRenderer();
    byName.SetGpuPreference(Lowercase(name)); // case-insensitive
    ::testing::internal::CaptureStderr();
    ASSERT_TRUE(byName.Initialize(nullptr, 4, 4)) << byName.GetInitMessage();
    std::string output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(byName.GetDeviceName(), name);
    EXPECT_NE(output.find("chosen by the GPU preference name '"), std::string::npos) << output;
    EXPECT_NE(output.find("(SetGpuPreference)"), std::string::npos) << output;
    EXPECT_EQ(Count(output, "cannot be used"), 0) << output;
    EXPECT_EQ(Count(output, "names no Vulkan device"), 0) << output;

    VulkanRenderer& byIndex = NewRenderer();
    byIndex.SetGpuPreference(std::to_string(index));
    ::testing::internal::CaptureStderr();
    ASSERT_TRUE(byIndex.Initialize(nullptr, 4, 4)) << byIndex.GetInitMessage();
    output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(byIndex.GetDeviceName(), name);
    EXPECT_NE(output.find("chosen by the GPU preference index " + std::to_string(index)), std::string::npos) << output;

    // It draws as usual
    ClearTo(byIndex, {1, 2, 3, 255});
    RenderFrame(byIndex);
    EXPECT_TRUE(IsSolid(Read(byIndex, 4, 4), 4, 4, {1, 2, 3, 255}));

    // An empty preference clears it: the default again
    byIndex.SetGpuPreference("");
    ::testing::internal::CaptureStderr();
    ASSERT_TRUE(byIndex.Initialize(nullptr, 4, 4)) << byIndex.GetInitMessage();
    output = ::testing::internal::GetCapturedStderr();
    if (Env("N2_VULKAN_GPU").empty())
    {
        EXPECT_NE(output.find("chosen by default"), std::string::npos) << output;
    }
}

TEST_F(VulkanTest, APreferenceNamingNoDeviceFallsBackToTheDefaultWithOneWarning)
{
    const std::vector<GPUInfo> gpus = R().GetCompatibleGPUs();
    const VkPhysicalDevice recommended = R().GetRecommendedGPU(gpus);
    std::string recommendedName;
    for (const GPUInfo& gpu : gpus)
    {
        if (gpu.device == recommended)
        {
            recommendedName = gpu.name;
        }
    }

    for (const std::string& preference : {std::string(Nonsense), std::string("999")})
    {
        SCOPED_TRACE(preference);
        VulkanRenderer& renderer = NewRenderer();
        renderer.SetGpuPreference(preference);
        ::testing::internal::CaptureStderr();
        ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << "a preference never fails Initialize: "
                                                        << renderer.GetInitMessage();
        const std::string output = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(Count(output, "the GPU preference"), 1) << output;
        EXPECT_EQ(Count(output, "names no Vulkan device"), 1) << output;
        EXPECT_NE(output.find(preference), std::string::npos) << "the warning names the preference: " << output;
        EXPECT_NE(output.find("'" + recommendedName + "' instead"), std::string::npos)
            << "the warning names the device used: " << output;
        EXPECT_NE(output.find("chosen by default"), std::string::npos) << output;
        EXPECT_EQ(renderer.GetDeviceName(), recommendedName);
    }
}

TEST_F(VulkanTest, TheEnvironmentVariableN2VulkanGpuChoosesTheDeviceAndSetGpuPreferenceWins)
{
    const std::vector<GPUInfo> gpus = R().GetCompatibleGPUs();
    const int index = IndexInUse(R(), gpus);
    ASSERT_GE(index, 0);
    const std::string name = gpus[static_cast<std::size_t>(index)].name;

    {
        const ScopedEnvironment environment("N2_VULKAN_GPU", Lowercase(name));
        VulkanRenderer& renderer = NewRenderer();
        ::testing::internal::CaptureStderr();
        ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();
        const std::string output = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(renderer.GetDeviceName(), name);
        EXPECT_NE(output.find("chosen by the GPU preference name '"), std::string::npos) << output;
        EXPECT_NE(output.find("(N2_VULKAN_GPU)"), std::string::npos) << output;

        // SetGpuPreference wins over the variable: its nonsense gives the one warning, naming SetGpuPreference
        VulkanRenderer& overridden = NewRenderer();
        overridden.SetGpuPreference(Nonsense);
        ::testing::internal::CaptureStderr();
        ASSERT_TRUE(overridden.Initialize(nullptr, 4, 4)) << overridden.GetInitMessage();
        const std::string overriddenOutput = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(Count(overriddenOutput, "names no Vulkan device"), 1) << overriddenOutput;
        EXPECT_NE(overriddenOutput.find("(SetGpuPreference)"), std::string::npos) << overriddenOutput;
    }
    {
        const ScopedEnvironment environment("N2_VULKAN_GPU", std::to_string(index));
        VulkanRenderer& renderer = NewRenderer();
        ::testing::internal::CaptureStderr();
        ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();
        const std::string output = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(renderer.GetDeviceName(), name);
        EXPECT_NE(output.find("chosen by the GPU preference index " + std::to_string(index) + " (N2_VULKAN_GPU)"),
                  std::string::npos)
            << output;
    }
    {
        const ScopedEnvironment environment("N2_VULKAN_GPU", Nonsense);
        VulkanRenderer& renderer = NewRenderer();
        ::testing::internal::CaptureStderr();
        ASSERT_TRUE(renderer.Initialize(nullptr, 4, 4)) << renderer.GetInitMessage();
        const std::string output = ::testing::internal::GetCapturedStderr();
        EXPECT_EQ(Count(output, "the GPU preference"), 1) << output;
        EXPECT_NE(output.find("(N2_VULKAN_GPU)"), std::string::npos) << output;
    }
}
