#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

// The swapchain decisions of the Vulkan backend (stage F of #43) as PURE functions over plain Vulkan structs: no device,
// no surface, no call into Vulkan. The renderer (renderer/src/vulkan/VulkanSwapchain.cpp) queries the surface and hands
// the results here, and tests/renderer/vulkan/VulkanSwapchainChoiceTests.cpp checks every rule with tables of fake
// capabilities, on every leg that builds the backend (no driver needed). docs/application.html#vulkan-renderer explains
// each choice.

namespace Renderer::Vulkan
{
    /// The value of VkSurfaceCapabilitiesKHR::currentExtent's axes when the surface size is decided by the swapchain
    inline constexpr uint32_t kUndefinedSurfaceExtent = 0xFFFFFFFFu;

    /// Whether `format` is one of the *_SRGB formats (the image encodes on write and decodes on read)
    [[nodiscard]] bool IsSrgbFormat(VkFormat format);

    struct SurfaceFormatChoice
    {
        bool found = false; // false: nothing usable was offered (an empty list, or no SRGB_NONLINEAR entry)
        VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        /// Only *_SRGB formats were offered, so the blit encodes the already encoded bytes a second time (too bright)
        bool doubleEncoded = false;
    };

    /// The swapchain format. The offscreen image is R8G8B8A8_UNORM and already holds the final encoded bytes, so the
    /// swapchain must take them unchanged: B8G8R8A8_UNORM, then R8G8B8A8_UNORM, then A8B8G8R8_UNORM_PACK32, then the 10-bit
    /// UNORM formats (A2B10G10R10, A2R10G10B10), each only with VK_COLOR_SPACE_SRGB_NONLINEAR_KHR. A single
    /// VK_FORMAT_UNDEFINED entry means "any format": B8G8R8A8_UNORM. An *_SRGB format is NEVER chosen while one of those
    /// UNORM formats is offered; when only sRGB ones are (with SRGB_NONLINEAR), the first of them is chosen and
    /// `doubleEncoded` says so. Anything else (an empty list, only other colour spaces, only formats outside these
    /// lists) finds nothing.
    [[nodiscard]] SurfaceFormatChoice ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats);

    /// vsync on: FIFO, whatever is offered (FIFO is always supported). vsync off: MAILBOX if offered, else IMMEDIATE if
    /// offered, else FIFO
    [[nodiscard]] VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync);

    struct ExtentChoice
    {
        bool minimised = false; // a 0 in either axis: nothing to present, and no swapchain may be made at this size
        VkExtent2D extent{0, 0};
    };

    /// A 0 width or height
    [[nodiscard]] bool IsMinimised(VkExtent2D extent);

    /// The swapchain size: capabilities.currentExtent when it is defined (its width is not kUndefinedSurfaceExtent),
    /// else the framebuffer size clamped to [minImageExtent, maxImageExtent] per axis. Minimised when the framebuffer or
    /// the result has a 0 in either axis (never clamped up to 1)
    [[nodiscard]] ExtentChoice ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t framebufferWidth,
                                            uint32_t framebufferHeight);

    /// minImageCount + 1, clamped to maxImageCount when that is not 0 (0 means no maximum)
    [[nodiscard]] uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& capabilities);

    /// OPAQUE when supported, else the first supported bit of PRE_MULTIPLIED, POST_MULTIPLIED, INHERIT (OPAQUE when none is)
    [[nodiscard]] VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported);

    /// NEAREST when the offscreen image and the swapchain image are the same size (a 1:1 copy); LINEAR when they differ and
    /// the offscreen format can be filtered linearly, else NEAREST
    [[nodiscard]] VkFilter ChooseBlitFilter(VkExtent2D source, VkExtent2D destination, bool linearFilterSupported);

    /// What Present does next with the swapchain
    enum class SwapchainAction
    {
        Continue,     // go on: acquire, blit and present (after a present: nothing to do)
        RecreateNow,  // recreate the swapchain now; after an acquire: this frame is not presented
        RecreateNext, // present this frame (the image was acquired), and recreate before the next present
        Skip,         // do nothing this frame (minimised, or the acquire gave no image), and do not recreate
        DeviceLost,   // the device is gone: stop rendering (frames are skipped)
        SurfaceLost,  // the surface is gone: recreate the surface and the swapchain once
    };

    [[nodiscard]] const char* ToString(SwapchainAction action);

    /// Before acquiring: Skip when the framebuffer is minimised; RecreateNow when there is no swapchain, a recreate is
    /// pending (a resize, a vsync change, an earlier SUBOPTIMAL or OUT_OF_DATE), or the framebuffer is not the size the
    /// swapchain was made for; else Continue
    [[nodiscard]] SwapchainAction DecideBeforeAcquire(bool hasSwapchain, bool recreatePending, VkExtent2D framebuffer,
                                                      VkExtent2D swapchainFramebuffer);

    /// After vkAcquireNextImageKHR: SUCCESS Continue; SUBOPTIMAL RecreateNext (the image is usable); OUT_OF_DATE
    /// RecreateNow (no image); DEVICE_LOST DeviceLost; SURFACE_LOST SurfaceLost; anything else (TIMEOUT, NOT_READY, an
    /// out-of-memory error) Skip
    [[nodiscard]] SwapchainAction DecideAfterAcquire(VkResult result);

    /// After vkQueuePresentKHR: SUCCESS Continue; SUBOPTIMAL and OUT_OF_DATE RecreateNext; DEVICE_LOST DeviceLost;
    /// SURFACE_LOST SurfaceLost; anything else Continue (the frame is lost, the next one tries again)
    [[nodiscard]] SwapchainAction DecideAfterPresent(VkResult result);
}
