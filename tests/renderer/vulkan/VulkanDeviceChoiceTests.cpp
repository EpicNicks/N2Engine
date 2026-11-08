#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <renderer/vulkan/VulkanDeviceChoice.hpp>

// The Vulkan backend's device choice (stage F of #43) is a pure function over plain candidate descriptions
// (renderer/vulkan/VulkanDeviceChoice.hpp): these tables run with no Vulkan device, on every leg that builds the backend.
// The same choice on a real instance (lavapipe) is VulkanGpuSelectionTests.cpp; two real GPUs are checked by hand
// (docs/gpu-vulkan-checklist.html).

using namespace Renderer::Vulkan;

namespace
{
    constexpr uint64_t GiB = 1024ull * 1024ull * 1024ull;

    /// A device that qualifies headless and with a surface
    DeviceCandidate Device(uint32_t index, std::string name, VkPhysicalDeviceType type, uint64_t memory = 0)
    {
        DeviceCandidate candidate;
        candidate.index = index;
        candidate.name = std::move(name);
        candidate.type = type;
        candidate.apiVersion = VK_API_VERSION_1_3;
        candidate.deviceLocalBytes = memory;
        candidate.hasGraphics = true;
        candidate.formatsOk = true;
        candidate.presentOk = true;
        candidate.swapchainExtOk = true;
        return candidate;
    }

    constexpr auto Discrete = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    constexpr auto Integrated = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    constexpr auto Virtual = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    constexpr auto Cpu = VK_PHYSICAL_DEVICE_TYPE_CPU;
    constexpr auto Other = VK_PHYSICAL_DEVICE_TYPE_OTHER;

    GpuPreference None()
    {
        return {};
    }

    GpuPreference Index(uint32_t index)
    {
        GpuPreference preference;
        preference.kind = GpuPreference::Kind::Index;
        preference.index = index;
        return preference;
    }

    GpuPreference Name(std::string name)
    {
        GpuPreference preference;
        preference.kind = GpuPreference::Kind::Name;
        preference.name = std::move(name);
        return preference;
    }

    /// The name of the device ChooseDevice picks, or "(none)"
    std::string Chosen(const std::vector<DeviceCandidate>& candidates, bool needsPresent, const GpuPreference& preference)
    {
        const DeviceChoice choice = ChooseDevice(candidates, needsPresent, preference);
        return choice.found ? candidates[choice.position].name : std::string("(none)");
    }
}

TEST(VulkanDeviceChoiceTest, TheDefaultPrefersDiscreteThenIntegratedThenVirtualThenCpuThenOther)
{
    const std::vector<DeviceCandidate> all = {
        Device(0, "llvmpipe", Cpu), Device(1, "Intel UHD", Integrated), Device(2, "Other", Other),
        Device(3, "NVIDIA RTX 3060", Discrete), Device(4, "Virtio", Virtual),
    };
    EXPECT_EQ(Chosen(all, false, None()), "NVIDIA RTX 3060");
    EXPECT_EQ(Chosen(all, true, None()), "NVIDIA RTX 3060");
    EXPECT_EQ(Chosen({all[0], all[1], all[2], all[4]}, false, None()), "Intel UHD");
    EXPECT_EQ(Chosen({all[0], all[2], all[4]}, false, None()), "Virtio");
    EXPECT_EQ(Chosen({all[2], all[0]}, false, None()), "llvmpipe");
    EXPECT_EQ(Chosen({all[2]}, false, None()), "Other");
    EXPECT_EQ(Chosen({}, false, None()), "(none)");
    EXPECT_FALSE(ChooseDevice({all[3]}, false, None()).byPreference);
    EXPECT_TRUE(ChooseDevice({all[3]}, false, None()).warning.empty());

    EXPECT_GT(ScoreDeviceType(Discrete), ScoreDeviceType(Integrated));
    EXPECT_GT(ScoreDeviceType(Integrated), ScoreDeviceType(Virtual));
    EXPECT_GT(ScoreDeviceType(Virtual), ScoreDeviceType(Cpu));
    EXPECT_GT(ScoreDeviceType(Cpu), ScoreDeviceType(Other));
    EXPECT_STREQ(DeviceTypeName(Discrete), "discrete GPU");
    EXPECT_STREQ(DeviceTypeName(Cpu), "CPU");
}

TEST(VulkanDeviceChoiceTest, TiesAreBrokenByMoreDeviceLocalMemoryThenEnumerationOrder)
{
    const std::vector<DeviceCandidate> byMemory = {
        Device(0, "small", Discrete, 4 * GiB), Device(1, "big", Discrete, 12 * GiB), Device(2, "mid", Discrete, 8 * GiB)};
    EXPECT_EQ(Chosen(byMemory, false, None()), "big");

    const std::vector<DeviceCandidate> byOrder = {
        Device(0, "first", Integrated, 2 * GiB), Device(1, "second", Integrated, 2 * GiB)};
    EXPECT_EQ(Chosen(byOrder, false, None()), "first");
    // The enumeration index decides, not the position in the vector
    EXPECT_EQ(Chosen({Device(5, "late", Integrated, 2 * GiB), Device(3, "early", Integrated, 2 * GiB)}, false, None()),
              "early");
    // The type beats memory
    EXPECT_EQ(Chosen({Device(0, "huge integrated", Integrated, 64 * GiB), Device(1, "discrete", Discrete, 1 * GiB)}, false,
                     None()),
              "discrete");
}

TEST(VulkanDeviceChoiceTest, PresentSupportAndTheSwapchainExtensionCountOnlyWithASurface)
{
    DeviceCandidate noPresent = Device(0, "discrete without present", Discrete);
    noPresent.presentOk = false;
    DeviceCandidate noSwapchain = Device(1, "discrete without swapchain", Discrete);
    noSwapchain.swapchainExtOk = false;
    const DeviceCandidate integrated = Device(2, "integrated", Integrated);
    const std::vector<DeviceCandidate> devices = {noPresent, noSwapchain, integrated};

    // Headless: both discrete devices qualify; the first wins
    EXPECT_EQ(Chosen(devices, false, None()), "discrete without present");
    EXPECT_TRUE(Qualifies(noPresent, false));
    EXPECT_TRUE(Qualifies(noSwapchain, false));
    // With a surface: neither qualifies, so the integrated GPU is chosen
    EXPECT_EQ(Chosen(devices, true, None()), "integrated");
    EXPECT_FALSE(Qualifies(noPresent, true));
    EXPECT_FALSE(Qualifies(noSwapchain, true));
    EXPECT_NE(WhyNotQualified(noSwapchain, true).find("VK_KHR_swapchain"), std::string::npos);
    EXPECT_NE(WhyNotQualified(noPresent, true).find("presents"), std::string::npos);
    EXPECT_TRUE(WhyNotQualified(integrated, true).empty());
    // With a surface and nothing that presents: no device
    EXPECT_EQ(Chosen({noPresent, noSwapchain}, true, None()), "(none)");
}

TEST(VulkanDeviceChoiceTest, DevicesWithoutGraphicsFormatsOrVulkan11NeverQualify)
{
    DeviceCandidate noGraphics = Device(0, "compute only", Discrete);
    noGraphics.hasGraphics = false;
    DeviceCandidate noFormats = Device(1, "no formats", Discrete);
    noFormats.formatsOk = false;
    DeviceCandidate old = Device(2, "Vulkan 1.0", Discrete);
    old.apiVersion = VK_API_VERSION_1_0;
    const DeviceCandidate cpu = Device(3, "lavapipe", Cpu);
    for (const bool surface : {false, true})
    {
        EXPECT_EQ(Chosen({noGraphics, noFormats, old, cpu}, surface, None()), "lavapipe");
        EXPECT_EQ(Chosen({noGraphics, noFormats, old}, surface, None()), "(none)");
    }
    EXPECT_NE(WhyNotQualified(old, false).find("1.1"), std::string::npos);
    EXPECT_NE(WhyNotQualified(noGraphics, false).find("graphics"), std::string::npos);
}

TEST(VulkanDeviceChoiceTest, AnIndexPreferenceNamingAQualifyingDeviceWins)
{
    const std::vector<DeviceCandidate> devices = {Device(0, "Intel UHD 770", Integrated, 1 * GiB),
                                                  Device(1, "NVIDIA GeForce RTX 3060", Discrete, 12 * GiB)};
    const DeviceChoice choice = ChooseDevice(devices, true, Index(0));
    ASSERT_TRUE(choice.found);
    EXPECT_EQ(devices[choice.position].name, "Intel UHD 770");
    EXPECT_TRUE(choice.byPreference);
    EXPECT_TRUE(choice.warning.empty());
    EXPECT_EQ(Chosen(devices, true, Index(1)), "NVIDIA GeForce RTX 3060");
}

TEST(VulkanDeviceChoiceTest, ANamePreferenceIsACaseInsensitiveSubstring)
{
    const std::vector<DeviceCandidate> devices = {Device(0, "NVIDIA GeForce RTX 3060", Discrete, 12 * GiB),
                                                  Device(1, "Intel(R) UHD Graphics 770", Integrated, 1 * GiB),
                                                  Device(2, "llvmpipe (LLVM 17.0.6, 256 bits)", Cpu)};
    EXPECT_EQ(Chosen(devices, false, Name("intel")), "Intel(R) UHD Graphics 770");
    EXPECT_EQ(Chosen(devices, false, Name("UHD GRAPHICS")), "Intel(R) UHD Graphics 770");
    EXPECT_EQ(Chosen(devices, false, Name("LLVMpipe")), "llvmpipe (LLVM 17.0.6, 256 bits)");
    EXPECT_EQ(Chosen(devices, false, Name("rtx")), "NVIDIA GeForce RTX 3060");
    const DeviceChoice choice = ChooseDevice(devices, false, Name("Intel"));
    EXPECT_TRUE(choice.byPreference);
    EXPECT_TRUE(choice.warning.empty());

    // A name part matching several: the default order among them (here the one with more memory)
    const std::vector<DeviceCandidate> twins = {Device(0, "AMD Radeon RX 6600", Discrete, 8 * GiB),
                                                Device(1, "AMD Radeon RX 6800", Discrete, 16 * GiB),
                                                Device(2, "NVIDIA RTX 4090", Discrete, 24 * GiB)};
    EXPECT_EQ(Chosen(twins, false, Name("radeon")), "AMD Radeon RX 6800");
}

TEST(VulkanDeviceChoiceTest, APreferenceThatNamesNothingFallsBackToTheDefaultWithAWarning)
{
    const std::vector<DeviceCandidate> devices = {Device(0, "Intel UHD", Integrated), Device(1, "NVIDIA RTX", Discrete)};

    const DeviceChoice outOfRange = ChooseDevice(devices, false, Index(7));
    ASSERT_TRUE(outOfRange.found);
    EXPECT_EQ(devices[outOfRange.position].name, "NVIDIA RTX");
    EXPECT_FALSE(outOfRange.byPreference);
    EXPECT_NE(outOfRange.warning.find("index 7"), std::string::npos) << outOfRange.warning;
    EXPECT_NE(outOfRange.warning.find("names no Vulkan device"), std::string::npos) << outOfRange.warning;
    EXPECT_NE(outOfRange.warning.find("'NVIDIA RTX' instead"), std::string::npos) << outOfRange.warning;

    const DeviceChoice unknown = ChooseDevice(devices, false, Name("no-such-gpu"));
    ASSERT_TRUE(unknown.found);
    EXPECT_EQ(devices[unknown.position].name, "NVIDIA RTX");
    EXPECT_NE(unknown.warning.find("name 'no-such-gpu'"), std::string::npos) << unknown.warning;
    EXPECT_NE(unknown.warning.find("'NVIDIA RTX' instead"), std::string::npos) << unknown.warning;

    const DeviceChoice huge = ChooseDevice(devices, false, Index(std::numeric_limits<uint32_t>::max()));
    EXPECT_EQ(devices[huge.position].name, "NVIDIA RTX");
    EXPECT_FALSE(huge.warning.empty());
}

TEST(VulkanDeviceChoiceTest, APreferenceNamingADeviceThatDoesNotQualifyFallsBackWithTheReason)
{
    DeviceCandidate noPresent = Device(0, "Discrete without present", Discrete);
    noPresent.presentOk = false;
    const std::vector<DeviceCandidate> devices = {noPresent, Device(1, "Integrated", Integrated)};

    const DeviceChoice byIndex = ChooseDevice(devices, true, Index(0));
    ASSERT_TRUE(byIndex.found);
    EXPECT_EQ(devices[byIndex.position].name, "Integrated");
    EXPECT_FALSE(byIndex.byPreference);
    EXPECT_NE(byIndex.warning.find("cannot be used"), std::string::npos) << byIndex.warning;
    EXPECT_NE(byIndex.warning.find("'Integrated' instead"), std::string::npos) << byIndex.warning;

    const DeviceChoice byName = ChooseDevice(devices, true, Name("without present"));
    EXPECT_EQ(devices[byName.position].name, "Integrated");
    EXPECT_NE(byName.warning.find("'Discrete without present'"), std::string::npos) << byName.warning;

    // Headless the same device qualifies, so the same preference is followed
    EXPECT_EQ(Chosen(devices, false, Index(0)), "Discrete without present");

    // No qualifying device at all: not found (Initialize fails with NoSuitableDevice), preference or not
    EXPECT_FALSE(ChooseDevice({noPresent}, true, Index(0)).found);
}

TEST(VulkanDeviceChoiceTest, ParsingTheGpuPreferenceText)
{
    EXPECT_EQ(ParseGpuPreference("").kind, GpuPreference::Kind::None);
    EXPECT_EQ(ParseGpuPreference("   ").kind, GpuPreference::Kind::None);

    const GpuPreference zero = ParseGpuPreference("0");
    EXPECT_EQ(zero.kind, GpuPreference::Kind::Index);
    EXPECT_EQ(zero.index, 0u);
    const GpuPreference one = ParseGpuPreference(" 12 ");
    EXPECT_EQ(one.kind, GpuPreference::Kind::Index);
    EXPECT_EQ(one.index, 12u);
    const GpuPreference huge = ParseGpuPreference("99999999999999999999");
    EXPECT_EQ(huge.kind, GpuPreference::Kind::Index);
    EXPECT_EQ(huge.index, std::numeric_limits<uint32_t>::max());

    const GpuPreference name = ParseGpuPreference("  NVIDIA RTX ");
    EXPECT_EQ(name.kind, GpuPreference::Kind::Name);
    EXPECT_EQ(name.name, "NVIDIA RTX");
    EXPECT_EQ(ParseGpuPreference("-1").kind, GpuPreference::Kind::Name) << "not only digits: a name part";
    EXPECT_EQ(ParseGpuPreference("1a").kind, GpuPreference::Kind::Name);
    EXPECT_EQ(ParseGpuPreference("3060").kind, GpuPreference::Kind::Index) << "only digits is always an index";

    EXPECT_EQ(DescribeGpuPreference(ParseGpuPreference("2")), "index 2");
    EXPECT_EQ(DescribeGpuPreference(ParseGpuPreference("amd")), "name 'amd'");
    EXPECT_EQ(DescribeGpuPreference(ParseGpuPreference("")), "none");
}
