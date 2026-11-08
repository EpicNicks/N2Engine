#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <renderer/vulkan/VulkanSwapchainChoice.hpp>

// The swapchain choices of the Vulkan backend (stage F of #43) are pure functions over plain structs
// (renderer/vulkan/VulkanSwapchainChoice.hpp): these tables run with no Vulkan device, on every leg that builds the
// backend (both Windows legs and both Linux Clang legs). What a real driver and window do with the choices is checked by
// hand (docs/gpu-vulkan-checklist.html); the swapchain path itself runs on lavapipe's headless surface
// (VulkanHeadlessSwapchainTests.cpp).

using namespace Renderer::Vulkan;

namespace
{
    constexpr VkColorSpaceKHR Srgb = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    constexpr VkColorSpaceKHR Linear = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;

    VkSurfaceFormatKHR F(VkFormat format, VkColorSpaceKHR space = Srgb)
    {
        return {format, space};
    }

    struct FormatCase
    {
        const char* name;
        std::vector<VkSurfaceFormatKHR> offered;
        bool found;
        VkFormat format;
        bool doubleEncoded;
    };

    VkSurfaceCapabilitiesKHR Caps(uint32_t currentWidth, uint32_t currentHeight, VkExtent2D minExtent = {1, 1},
                                  VkExtent2D maxExtent = {16384, 16384}, uint32_t minImages = 2, uint32_t maxImages = 8)
    {
        VkSurfaceCapabilitiesKHR caps{};
        caps.currentExtent = {currentWidth, currentHeight};
        caps.minImageExtent = minExtent;
        caps.maxImageExtent = maxExtent;
        caps.minImageCount = minImages;
        caps.maxImageCount = maxImages;
        return caps;
    }

    constexpr uint32_t U = kUndefinedSurfaceExtent;
}

TEST(VulkanSwapchainChoiceTest, SurfaceFormatTable)
{
    const std::vector<FormatCase> cases = {
        {"empty list: nothing", {}, false, VK_FORMAT_UNDEFINED, false},
        {"one UNDEFINED entry means any: B8G8R8A8_UNORM", {F(VK_FORMAT_UNDEFINED)}, true, VK_FORMAT_B8G8R8A8_UNORM, false},
        {"B8G8R8A8_UNORM alone", {F(VK_FORMAT_B8G8R8A8_UNORM)}, true, VK_FORMAT_B8G8R8A8_UNORM, false},
        {"BGRA UNORM over RGBA UNORM, either order",
         {F(VK_FORMAT_R8G8B8A8_UNORM), F(VK_FORMAT_B8G8R8A8_UNORM)}, true, VK_FORMAT_B8G8R8A8_UNORM, false},
        {"BGRA UNORM over RGBA UNORM, other order",
         {F(VK_FORMAT_B8G8R8A8_UNORM), F(VK_FORMAT_R8G8B8A8_UNORM)}, true, VK_FORMAT_B8G8R8A8_UNORM, false},
        {"UNORM over sRGB: sRGB first (the usual desktop list)",
         {F(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_B8G8R8A8_UNORM)}, true, VK_FORMAT_B8G8R8A8_UNORM, false},
        {"UNORM over sRGB: sRGB last", {F(VK_FORMAT_B8G8R8A8_UNORM), F(VK_FORMAT_B8G8R8A8_SRGB)}, true,
         VK_FORMAT_B8G8R8A8_UNORM, false},
        {"RGBA UNORM over BGRA sRGB", {F(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_R8G8B8A8_UNORM)}, true,
         VK_FORMAT_R8G8B8A8_UNORM, false},
        {"A8B8G8R8 UNORM over every sRGB",
         {F(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_R8G8B8A8_SRGB), F(VK_FORMAT_A8B8G8R8_UNORM_PACK32)}, true,
         VK_FORMAT_A8B8G8R8_UNORM_PACK32, false},
        {"10-bit UNORM over sRGB", {F(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_A2B10G10R10_UNORM_PACK32)}, true,
         VK_FORMAT_A2B10G10R10_UNORM_PACK32, false},
        {"8-bit UNORM over 10-bit UNORM", {F(VK_FORMAT_A2R10G10B10_UNORM_PACK32), F(VK_FORMAT_R8G8B8A8_UNORM)}, true,
         VK_FORMAT_R8G8B8A8_UNORM, false},
        {"only sRGB: the first one, double encoded", {F(VK_FORMAT_R8G8B8A8_SRGB), F(VK_FORMAT_B8G8R8A8_SRGB)}, true,
         VK_FORMAT_R8G8B8A8_SRGB, true},
        {"only sRGB, one entry", {F(VK_FORMAT_B8G8R8A8_SRGB)}, true, VK_FORMAT_B8G8R8A8_SRGB, true},
        {"UNORM only in the wrong colour space, sRGB in the right one: the sRGB one",
         {F(VK_FORMAT_B8G8R8A8_UNORM, Linear), F(VK_FORMAT_B8G8R8A8_SRGB)}, true, VK_FORMAT_B8G8R8A8_SRGB, true},
        {"the wrong colour space only: nothing", {F(VK_FORMAT_B8G8R8A8_UNORM, Linear), F(VK_FORMAT_B8G8R8A8_SRGB, Linear)},
         false, VK_FORMAT_UNDEFINED, false},
        {"the right format in two colour spaces: SRGB_NONLINEAR",
         {F(VK_FORMAT_B8G8R8A8_UNORM, Linear), F(VK_FORMAT_B8G8R8A8_UNORM)}, true, VK_FORMAT_B8G8R8A8_UNORM, false},
        {"only formats outside the lists: nothing", {F(VK_FORMAT_R16G16B16A16_SFLOAT), F(VK_FORMAT_R5G6B5_UNORM_PACK16)},
         false, VK_FORMAT_UNDEFINED, false},
        {"UNDEFINED among others is not 'any': the UNORM one", {F(VK_FORMAT_UNDEFINED), F(VK_FORMAT_R8G8B8A8_UNORM)}, true,
         VK_FORMAT_R8G8B8A8_UNORM, false},
    };
    for (const FormatCase& c : cases)
    {
        SCOPED_TRACE(c.name);
        const SurfaceFormatChoice choice = ChooseSurfaceFormat(c.offered);
        EXPECT_EQ(choice.found, c.found);
        EXPECT_EQ(choice.doubleEncoded, c.doubleEncoded);
        if (c.found)
        {
            EXPECT_EQ(choice.format.format, c.format);
            EXPECT_EQ(choice.format.colorSpace, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        }
    }
}

TEST(VulkanSwapchainChoiceTest, AnSrgbFormatIsNeverChosenWhileAUnormOneIsOffered)
{
    // Every order of a list holding the three 8-bit sRGB formats and one UNORM format: the UNORM one wins every time
    const std::vector<VkFormat> unorms = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                          VK_FORMAT_A8B8G8R8_UNORM_PACK32};
    for (const VkFormat unorm : unorms)
    {
        for (std::size_t position = 0; position <= 3; ++position)
        {
            std::vector<VkSurfaceFormatKHR> list = {F(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_R8G8B8A8_SRGB),
                                                    F(VK_FORMAT_A8B8G8R8_SRGB_PACK32)};
            list.insert(list.begin() + static_cast<std::ptrdiff_t>(position), F(unorm));
            const SurfaceFormatChoice choice = ChooseSurfaceFormat(list);
            ASSERT_TRUE(choice.found);
            EXPECT_EQ(choice.format.format, unorm) << "UNORM format " << unorm << " at position " << position;
            EXPECT_FALSE(IsSrgbFormat(choice.format.format));
            EXPECT_FALSE(choice.doubleEncoded);
        }
    }
}

TEST(VulkanSwapchainChoiceTest, SrgbFormatsAreRecognised)
{
    EXPECT_TRUE(IsSrgbFormat(VK_FORMAT_B8G8R8A8_SRGB));
    EXPECT_TRUE(IsSrgbFormat(VK_FORMAT_R8G8B8A8_SRGB));
    EXPECT_TRUE(IsSrgbFormat(VK_FORMAT_A8B8G8R8_SRGB_PACK32));
    EXPECT_FALSE(IsSrgbFormat(VK_FORMAT_B8G8R8A8_UNORM));
    EXPECT_FALSE(IsSrgbFormat(VK_FORMAT_R8G8B8A8_UNORM));
    EXPECT_FALSE(IsSrgbFormat(VK_FORMAT_UNDEFINED));
}

TEST(VulkanSwapchainChoiceTest, PresentModeTable)
{
    struct Case
    {
        const char* name;
        std::vector<VkPresentModeKHR> offered;
        bool vsync;
        VkPresentModeKHR expected;
    };
    const std::vector<Case> cases = {
        {"vsync on, everything offered: FIFO",
         {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR,
          VK_PRESENT_MODE_FIFO_RELAXED_KHR},
         true, VK_PRESENT_MODE_FIFO_KHR},
        {"vsync on, only MAILBOX listed: still FIFO (always supported)", {VK_PRESENT_MODE_MAILBOX_KHR}, true,
         VK_PRESENT_MODE_FIFO_KHR},
        {"vsync on, empty: FIFO", {}, true, VK_PRESENT_MODE_FIFO_KHR},
        {"vsync off: MAILBOX first", {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR},
         false, VK_PRESENT_MODE_MAILBOX_KHR},
        {"vsync off, no MAILBOX: IMMEDIATE", {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}, false,
         VK_PRESENT_MODE_IMMEDIATE_KHR},
        {"vsync off, only FIFO: FIFO", {VK_PRESENT_MODE_FIFO_KHR}, false, VK_PRESENT_MODE_FIFO_KHR},
        {"vsync off, FIFO_RELAXED and FIFO: FIFO", {VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR}, false,
         VK_PRESENT_MODE_FIFO_KHR},
        {"vsync off, empty: FIFO", {}, false, VK_PRESENT_MODE_FIFO_KHR},
        {"vsync off, an unknown value: FIFO", {static_cast<VkPresentModeKHR>(12345)}, false, VK_PRESENT_MODE_FIFO_KHR},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(ChoosePresentMode(c.offered, c.vsync), c.expected);
    }
}

TEST(VulkanSwapchainChoiceTest, ExtentTable)
{
    struct Case
    {
        const char* name;
        VkSurfaceCapabilitiesKHR caps;
        uint32_t framebufferWidth;
        uint32_t framebufferHeight;
        bool minimised;
        VkExtent2D extent;
    };
    const std::vector<Case> cases = {
        {"a defined currentExtent wins over a different framebuffer", Caps(800, 600), 1024, 768, false, {800, 600}},
        {"a defined currentExtent equal to the framebuffer", Caps(1280, 720), 1280, 720, false, {1280, 720}},
        {"undefined currentExtent: the framebuffer size", Caps(U, U), 640, 480, false, {640, 480}},
        {"HiDPI: a framebuffer larger than the window's size is taken in pixels", Caps(U, U), 2560, 1440, false,
         {2560, 1440}},
        {"HiDPI with a defined currentExtent in pixels", Caps(3840, 2160), 3840, 2160, false, {3840, 2160}},
        {"width below the minimum is clamped up, height kept", Caps(U, U, {100, 50}, {4000, 3000}), 20, 300, false,
         {100, 300}},
        {"height below the minimum is clamped up, width kept", Caps(U, U, {100, 50}, {4000, 3000}), 300, 20, false,
         {300, 50}},
        {"width above the maximum is clamped down, height kept", Caps(U, U, {1, 1}, {1000, 800}), 5000, 600, false,
         {1000, 600}},
        {"height above the maximum is clamped down, width kept", Caps(U, U, {1, 1}, {1000, 800}), 600, 5000, false,
         {600, 800}},
        {"both clamped, one up and one down", Caps(U, U, {64, 64}, {512, 512}), 10, 9000, false, {64, 512}},
        {"currentExtent 0x0: minimised", Caps(0, 0), 0, 0, true, {0, 0}},
        {"currentExtent 0 wide: minimised", Caps(0, 600), 800, 600, true, {0, 0}},
        {"currentExtent 0 high: minimised", Caps(800, 0), 800, 600, true, {0, 0}},
        {"framebuffer 0xN with a defined currentExtent: minimised", Caps(800, 600), 0, 600, true, {0, 0}},
        {"framebuffer Nx0, undefined currentExtent: minimised, never clamped up to 1", Caps(U, U, {1, 1}), 800, 0, true,
         {0, 0}},
        {"framebuffer 0x0, undefined currentExtent: minimised", Caps(U, U, {1, 1}), 0, 0, true, {0, 0}},
        {"a 0 maximum (some drivers while minimised): minimised", Caps(U, U, {0, 0}, {0, 0}), 800, 600, true, {0, 0}},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        const ExtentChoice choice = ChooseExtent(c.caps, c.framebufferWidth, c.framebufferHeight);
        EXPECT_EQ(choice.minimised, c.minimised);
        EXPECT_EQ(choice.extent.width, c.extent.width);
        EXPECT_EQ(choice.extent.height, c.extent.height);
    }
}

TEST(VulkanSwapchainChoiceTest, MinimiseDetector)
{
    EXPECT_TRUE(IsMinimised({0, 0}));
    EXPECT_TRUE(IsMinimised({0, 600}));
    EXPECT_TRUE(IsMinimised({800, 0}));
    EXPECT_FALSE(IsMinimised({1, 1}));
    EXPECT_FALSE(IsMinimised({800, 600}));
}

TEST(VulkanSwapchainChoiceTest, ImageCountTable)
{
    struct Case
    {
        const char* name;
        uint32_t minImages;
        uint32_t maxImages;
        uint32_t expected;
    };
    const std::vector<Case> cases = {
        {"min + 1", 2, 8, 3},
        {"min + 1 again", 3, 8, 4},
        {"maxImageCount 0 means no maximum", 2, 0, 3},
        {"maxImageCount 0 with a large minimum", 7, 0, 8},
        {"min == max: max", 3, 3, 3},
        {"min + 1 > max: max", 2, 2, 2},
        {"min + 1 == max", 2, 3, 3},
        {"min 1", 1, 0, 2},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.name);
        EXPECT_EQ(ChooseImageCount(Caps(U, U, {1, 1}, {100, 100}, c.minImages, c.maxImages)), c.expected);
    }
}

TEST(VulkanSwapchainChoiceTest, CompositeAlphaAndBlitFilter)
{
    EXPECT_EQ(ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR),
              VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    EXPECT_EQ(ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR | VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR),
              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR);
    EXPECT_EQ(ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR), VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR);
    EXPECT_EQ(ChooseCompositeAlpha(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR), VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR);
    EXPECT_EQ(ChooseCompositeAlpha(0), VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);

    EXPECT_EQ(ChooseBlitFilter({64, 64}, {64, 64}, true), VK_FILTER_NEAREST) << "same size: a 1:1 copy";
    EXPECT_EQ(ChooseBlitFilter({64, 64}, {128, 96}, true), VK_FILTER_LINEAR) << "scaled: linear";
    EXPECT_EQ(ChooseBlitFilter({64, 64}, {128, 96}, false), VK_FILTER_NEAREST) << "scaled, no linear filtering";
    EXPECT_EQ(ChooseBlitFilter({64, 32}, {64, 64}, true), VK_FILTER_LINEAR) << "one axis scaled";
}

TEST(VulkanSwapchainChoiceTest, RecreateDecisionTable)
{
    constexpr VkExtent2D size{800, 600};
    // Before the acquire
    EXPECT_EQ(DecideBeforeAcquire(true, false, size, size), SwapchainAction::Continue);
    EXPECT_EQ(DecideBeforeAcquire(true, true, size, size), SwapchainAction::RecreateNow) << "a pending resize";
    EXPECT_EQ(DecideBeforeAcquire(false, false, size, size), SwapchainAction::RecreateNow) << "no swapchain yet";
    EXPECT_EQ(DecideBeforeAcquire(true, false, {1024, 600}, size), SwapchainAction::RecreateNow) << "a new width";
    EXPECT_EQ(DecideBeforeAcquire(true, false, {800, 601}, size), SwapchainAction::RecreateNow) << "a new height";
    EXPECT_EQ(DecideBeforeAcquire(true, true, {0, 0}, size), SwapchainAction::Skip) << "minimised beats pending";
    EXPECT_EQ(DecideBeforeAcquire(false, false, {0, 600}, size), SwapchainAction::Skip) << "minimised, no swapchain";
    EXPECT_EQ(DecideBeforeAcquire(true, false, {800, 0}, size), SwapchainAction::Skip);

    // After the acquire
    EXPECT_EQ(DecideAfterAcquire(VK_SUCCESS), SwapchainAction::Continue);
    EXPECT_EQ(DecideAfterAcquire(VK_SUBOPTIMAL_KHR), SwapchainAction::RecreateNext) << "present this image, then recreate";
    EXPECT_EQ(DecideAfterAcquire(VK_ERROR_OUT_OF_DATE_KHR), SwapchainAction::RecreateNow) << "no image: skip, recreate";
    EXPECT_EQ(DecideAfterAcquire(VK_ERROR_DEVICE_LOST), SwapchainAction::DeviceLost);
    EXPECT_EQ(DecideAfterAcquire(VK_ERROR_SURFACE_LOST_KHR), SwapchainAction::SurfaceLost);
    EXPECT_EQ(DecideAfterAcquire(VK_TIMEOUT), SwapchainAction::Skip);
    EXPECT_EQ(DecideAfterAcquire(VK_NOT_READY), SwapchainAction::Skip);
    EXPECT_EQ(DecideAfterAcquire(VK_ERROR_OUT_OF_HOST_MEMORY), SwapchainAction::Skip);

    // After the present
    EXPECT_EQ(DecideAfterPresent(VK_SUCCESS), SwapchainAction::Continue);
    EXPECT_EQ(DecideAfterPresent(VK_SUBOPTIMAL_KHR), SwapchainAction::RecreateNext);
    EXPECT_EQ(DecideAfterPresent(VK_ERROR_OUT_OF_DATE_KHR), SwapchainAction::RecreateNext);
    EXPECT_EQ(DecideAfterPresent(VK_ERROR_DEVICE_LOST), SwapchainAction::DeviceLost);
    EXPECT_EQ(DecideAfterPresent(VK_ERROR_SURFACE_LOST_KHR), SwapchainAction::SurfaceLost);
    EXPECT_EQ(DecideAfterPresent(VK_ERROR_OUT_OF_HOST_MEMORY), SwapchainAction::Continue);

    EXPECT_STREQ(ToString(SwapchainAction::RecreateNext), "RecreateNext");
}
