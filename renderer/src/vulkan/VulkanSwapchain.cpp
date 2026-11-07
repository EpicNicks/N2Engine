#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "renderer/vulkan/VulkanRenderer.hpp"
#include "renderer/vulkan/VulkanSwapchainChoice.hpp"

#include "VulkanSwapchain.hpp"

// Stage F of the Vulkan backend (#43): the window. The renderer still renders every frame into its own offscreen colour
// image (VulkanRenderer.cpp); with a surface, Present copies the last submitted frame to a swapchain image with
// vkCmdBlitImage and presents it. Every decision (format, present mode, extent, image count, what to do after an acquire
// or a present) is a pure function in VulkanChoice.cpp (renderer/vulkan/VulkanSwapchainChoice.hpp); this file only queries
// the surface, makes the objects and follows those decisions.
//
// Surface: from the renderer's OWN instance, through glfwCreateWindowSurface for a window, or vkCreateHeadlessSurfaceEXT
// for the test-only headless surface (SetHeadlessSurfaceForTest). Destroyed after the device and before the instance.
//
// Synchronisation of one Present, in the frame slot of the frame it presents (the last one EndFrame submitted):
//   1. wait for the slot's present fence (its previous blit), so its command buffer and image-available semaphore are free;
//   2. vkAcquireNextImageKHR signals the slot's image-available semaphore;
//   3. one submission waits on it at the TRANSFER stage, blits the offscreen colour image (it stays in TRANSFER_SRC_OPTIMAL)
//      to the swapchain image (UNDEFINED -> TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR), signals the render-finished semaphore
//      of THAT swapchain image and the slot's present fence;
//   4. vkQueuePresentKHR waits on the render-finished semaphore.
// The render-finished semaphores are per swapchain image, not per frame slot: a semaphore a present waits on may only be
// signalled again after that present is known to be done, which the reacquire of the same image guarantees (the
// validation layer reports a per-slot one: VUID-vkQueueSubmit-pSignalSemaphores-00067). The next frame's render pass
// writes the offscreen image again after the blit read it: its external dependency's source scope includes the TRANSFER
// stage, and both are on the one queue in submission order, so the write waits for the read.
//
// Recreation waits for the device to go idle (vkDeviceWaitIdle): it happens on a resize, a vsync change or an
// OUT_OF_DATE / SUBOPTIMAL result, rarely enough that the stall does not matter, and it guarantees nothing still uses the
// old swapchain, its images or semaphores when they are destroyed.

using namespace Renderer::Vulkan;

namespace
{
    constexpr uint64_t PresentTimeoutNanoseconds = 10ull * 1000ull * 1000ull * 1000ull;

    VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        return barrier;
    }

    bool InstanceHasExtension(const char* name)
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS || count == 0)
        {
            return false;
        }
        std::vector<VkExtensionProperties> extensions(count);
        const VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        {
            return false;
        }
        for (uint32_t i = 0; i < count && i < extensions.size(); ++i)
        {
            if (std::strcmp(extensions[i].extensionName, name) == 0)
            {
                return true;
            }
        }
        return false;
    }

    VkResult QueryFormats(VkPhysicalDevice device, VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR>& formats)
    {
        formats.clear();
        uint32_t count = 0;
        VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
        {
            return result;
        }
        formats.resize(count);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());
        formats.resize(std::min<std::size_t>(count, formats.size()));
        return result == VK_INCOMPLETE ? VK_SUCCESS : result;
    }

    VkResult QueryPresentModes(VkPhysicalDevice device, VkSurfaceKHR surface, std::vector<VkPresentModeKHR>& modes)
    {
        modes.clear();
        uint32_t count = 0;
        VkResult result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
        if (result != VK_SUCCESS || count == 0)
        {
            return result;
        }
        modes.resize(count);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());
        modes.resize(std::min<std::size_t>(count, modes.size()));
        return result == VK_INCOMPLETE ? VK_SUCCESS : result;
    }

    /// The 8-bit, 4-byte formats a captured image is converted from (the test hook only)
    bool IsCapturableFormat(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings and test hooks
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::SetVSync(bool enabled)
{
    std::lock_guard lock(_mutex);
    _vsyncSetting = enabled;
    if (_vsync != enabled)
    {
        _vsync = enabled;
        _swapchain->recreatePending = true; // the present mode changes with the next swapchain
    }
}

bool VulkanRenderer::GetVSync() const
{
    std::lock_guard lock(_mutex);
    return _vsync;
}

bool VulkanRenderer::IsHeadlessSurfaceAvailableForTest()
{
    return InstanceHasExtension(VK_KHR_SURFACE_EXTENSION_NAME) &&
        InstanceHasExtension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
}

void VulkanRenderer::SetHeadlessSurfaceForTest(bool enabled, uint32_t framebufferWidth, uint32_t framebufferHeight)
{
    std::lock_guard lock(_mutex);
    _headlessSurfaceForTest = enabled;
    _testFramebuffer = {framebufferWidth, framebufferHeight};
}

void VulkanRenderer::SetFramebufferSizeForTest(uint32_t width, uint32_t height)
{
    std::lock_guard lock(_mutex);
    _testFramebuffer = {width, height};
}

void VulkanRenderer::ForceNextSwapchainResultForTest(SwapchainCallForTest call, VkResult result)
{
    std::lock_guard lock(_mutex);
    (call == SwapchainCallForTest::Acquire ? _forcedAcquireResult : _forcedPresentResult) = result;
}

void VulkanRenderer::SetPresentCaptureForTest(bool enabled)
{
    std::lock_guard lock(_mutex);
    _presentCaptureForTest = enabled;
}

SwapchainInfoForTest VulkanRenderer::GetSwapchainInfoForTest() const
{
    std::lock_guard lock(_mutex);
    const SwapchainState& state = *_swapchain;
    SwapchainInfoForTest info;
    info.hasSurface = _surface != VK_NULL_HANDLE;
    info.hasSwapchain = state.swapchain != VK_NULL_HANDLE;
    info.minimised = state.minimised;
    info.presentingStopped = state.presentingStopped;
    info.format = state.format;
    info.presentMode = state.presentMode;
    info.requestedImageCount = state.requestedImageCount;
    info.imageCount = static_cast<uint32_t>(state.images.size());
    info.extent = state.extent;
    info.createCount = state.createCount;
    info.presentCount = state.presentCount;
    info.surfaceCreateCount = state.surfaceCreateCount;
    if (_surface != VK_NULL_HANDLE && _physicalDevice != VK_NULL_HANDLE)
    {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &info.capabilities);
        QueryFormats(_physicalDevice, _surface, info.offeredFormats);
        QueryPresentModes(_physicalDevice, _surface, info.offeredPresentModes);
    }
    return info;
}

bool VulkanRenderer::ReadPresentedImageForTest(std::vector<std::uint8_t>& rgba, uint32_t& width, uint32_t& height)
{
    std::lock_guard lock(_mutex);
    const SwapchainState& state = *_swapchain;
    if (!state.captureValid || state.capture.mapped == nullptr || _device == VK_NULL_HANDLE)
    {
        return false;
    }
    vkQueueWaitIdle(_graphicsQueue); // the copy of the last present has finished
    if (!state.capture.coherent)
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = state.capture.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(_device, 1, &range);
    }

    width = state.captureExtent.width;
    height = state.captureExtent.height;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;
    rgba.resize(bytes);
    const auto* source = static_cast<const std::uint8_t*>(state.capture.mapped);
    // A8B8G8R8_PACK32 is R, G, B, A in memory on a little-endian host, like R8G8B8A8; B8G8R8A8 swaps R and B
    const bool bgra = state.captureFormat == VK_FORMAT_B8G8R8A8_UNORM || state.captureFormat == VK_FORMAT_B8G8R8A8_SRGB;
    for (std::size_t i = 0; i < bytes; i += 4)
    {
        rgba[i + 0] = source[i + (bgra ? 2 : 0)];
        rgba[i + 1] = source[i + 1];
        rgba[i + 2] = source[i + (bgra ? 0 : 2)];
        rgba[i + 3] = source[i + 3];
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Surface and present resources
// ---------------------------------------------------------------------------------------------------------------------

bool VulkanRenderer::CreateSurfaceLocked()
{
    if (_window != nullptr)
    {
        // From this renderer's own instance: the engine never makes a surface
        const VkResult result = glfwCreateWindowSurface(_instance, _window, nullptr, &_surface);
        if (result != VK_SUCCESS)
        {
            _surface = VK_NULL_HANDLE;
            if (result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
            {
                return Fail(InitStatus::NoSurface,
                            "glfwCreateWindowSurface: the window already has an OpenGL context or a surface "
                            "(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR); a Vulkan window must be made with "
                            "glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API)");
            }
            return Fail(InitStatus::NoSurface,
                        "glfwCreateWindowSurface failed (result " + std::to_string(result) + ")");
        }
    }
    else if (_headlessSurfaceForTest)
    {
        const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
            vkGetInstanceProcAddr(_instance, "vkCreateHeadlessSurfaceEXT"));
        VkHeadlessSurfaceCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
        const VkResult result = create != nullptr ? create(_instance, &info, nullptr, &_surface)
                                                  : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (result != VK_SUCCESS)
        {
            _surface = VK_NULL_HANDLE;
            return Fail(InitStatus::NoSurface,
                        "vkCreateHeadlessSurfaceEXT failed (result " + std::to_string(result) + ")");
        }
    }
    else
    {
        return true; // headless: no surface
    }
    ++_swapchain->surfaceCreateCount;
    return true;
}

void VulkanRenderer::DestroySurfaceLocked()
{
    if (_surface != VK_NULL_HANDLE && _instance != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
    }
    _surface = VK_NULL_HANDLE;
}

bool VulkanRenderer::CreatePresentResources()
{
    if (_surface == VK_NULL_HANDLE)
    {
        return true;
    }
    SwapchainState& state = *_swapchain;
    static_assert(std::tuple_size_v<decltype(state.slots)> == MAX_FRAMES_IN_FLIGHT);

    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> buffers{};
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = _commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    const VkResult allocated = vkAllocateCommandBuffers(_device, &allocateInfo, buffers.data());
    if (allocated != VK_SUCCESS)
    {
        return Fail(InitStatus::Failed,
                    "vkAllocateCommandBuffers (present) failed (result " + std::to_string(allocated) + ")");
    }
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        PresentSlot& slot = state.slots[i];
        slot.commandBuffer = buffers[i];

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &slot.imageAvailable) != VK_SUCCESS)
        {
            slot.imageAvailable = VK_NULL_HANDLE;
            return Fail(InitStatus::Failed, "vkCreateSemaphore (present) failed");
        }
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // the slot's first present must not wait
        if (vkCreateFence(_device, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS)
        {
            slot.fence = VK_NULL_HANDLE;
            return Fail(InitStatus::Failed, "vkCreateFence (present) failed");
        }
    }
    return true;
}

void VulkanRenderer::DestroyPresentResources()
{
    for (PresentSlot& slot : _swapchain->slots)
    {
        if (_device != VK_NULL_HANDLE)
        {
            if (slot.imageAvailable != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(_device, slot.imageAvailable, nullptr);
            }
            if (slot.fence != VK_NULL_HANDLE)
            {
                vkDestroyFence(_device, slot.fence, nullptr);
            }
        }
        slot = {}; // the command buffer is freed with the command pool
    }
}

bool VulkanRenderer::CreateCaptureBufferLocked(VkDeviceSize size)
{
    VulkanBuffer& buffer = _swapchain->capture;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_device, &info, nullptr, &buffer.buffer) != VK_SUCCESS)
    {
        buffer = {};
        return false;
    }
    buffer.size = size;
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(_device, buffer.buffer, &requirements);
    VkMemoryPropertyFlags chosen = 0;
    const bool allocated =
        AllocateMemory(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       buffer.memory, &chosen) ||
        AllocateMemory(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, buffer.memory, &chosen);
    if (!allocated || vkBindBufferMemory(_device, buffer.buffer, buffer.memory, 0) != VK_SUCCESS ||
        vkMapMemory(_device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped) != VK_SUCCESS)
    {
        buffer.mapped = nullptr;
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(_device, buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(_device, buffer.memory, nullptr);
        }
        buffer = {};
        return false;
    }
    buffer.coherent = (chosen & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The swapchain
// ---------------------------------------------------------------------------------------------------------------------

VkExtent2D VulkanRenderer::FramebufferSizeLocked() const
{
    if (_window == nullptr)
    {
        return _headlessSurfaceForTest ? _testFramebuffer : VkExtent2D{0, 0};
    }
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(_window, &width, &height); // pixels, so HiDPI windows get their real size
    return {static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0))};
}

VulkanRenderer::SwapchainResult VulkanRenderer::CreateSwapchainLocked(std::string& error)
{
    SwapchainState& state = *_swapchain;

    VkSurfaceCapabilitiesKHR capabilities{};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &capabilities);
    if (result == VK_ERROR_SURFACE_LOST_KHR)
    {
        return SwapchainResult::SurfaceLost;
    }
    if (result != VK_SUCCESS)
    {
        error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (result " + std::to_string(result) + ")";
        return SwapchainResult::Retry;
    }

    const VkExtent2D framebuffer = FramebufferSizeLocked();
    const ExtentChoice extent = ChooseExtent(capabilities, framebuffer.width, framebuffer.height);
    if (extent.minimised)
    {
        // A minimised window: keep what exists, present nothing, and recreate once the size is back
        state.minimised = true;
        state.recreatePending = true;
        return SwapchainResult::Minimised;
    }

    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> modes;
    result = QueryFormats(_physicalDevice, _surface, formats);
    if (result == VK_SUCCESS)
    {
        result = QueryPresentModes(_physicalDevice, _surface, modes);
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR)
    {
        return SwapchainResult::SurfaceLost;
    }
    if (result != VK_SUCCESS)
    {
        error = "querying the surface's formats and present modes failed (result " + std::to_string(result) + ")";
        return SwapchainResult::Retry;
    }

    // Only formats a blit can write (the one UNDEFINED entry means any format: checked once the format is known)
    const auto canBlitTo = [this](VkFormat format)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
    };
    std::vector<VkSurfaceFormatKHR> blittable;
    for (const VkSurfaceFormatKHR& format : formats)
    {
        if (format.format == VK_FORMAT_UNDEFINED || canBlitTo(format.format))
        {
            blittable.push_back(format);
        }
    }
    const SurfaceFormatChoice format = ChooseSurfaceFormat(blittable);
    if (!format.found || !canBlitTo(format.format.format))
    {
        error = "the window's surface offers no format the frame can be copied to (" + std::to_string(formats.size()) +
            " offered; an 8-bit or 10-bit UNORM or an 8-bit sRGB format in VK_COLOR_SPACE_SRGB_NONLINEAR_KHR that "
            "vkCmdBlitImage can write is needed)";
        return SwapchainResult::Unusable;
    }
    if (format.doubleEncoded)
    {
        LogOnce("the window's surface offers only sRGB formats (format " + std::to_string(format.format.format) +
                "): the frame's already encoded colours are encoded again, so the picture looks too bright");
    }
    if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
    {
        error = "the window's surface does not allow swapchain images to be written by a transfer (TRANSFER_DST)";
        return SwapchainResult::Unusable;
    }
    const bool capture = _presentCaptureForTest &&
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0 &&
        IsCapturableFormat(format.format.format);

    const VkPresentModeKHR presentMode = ChoosePresentMode(modes, _vsync);
    const uint32_t imageCount = ChooseImageCount(capabilities);

    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = _surface;
    info.minImageCount = imageCount;
    info.imageFormat = format.format.format;
    info.imageColorSpace = format.format.colorSpace;
    info.imageExtent = extent.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (capture)
    {
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // one queue family draws and presents (PickPhysicalDevice)
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = ChooseCompositeAlpha(capabilities.supportedCompositeAlpha);
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = state.swapchain;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(_device, &info, nullptr, &swapchain);
    // The old swapchain is retired by that call whether or not it succeeded, and nothing uses it any more (the callers
    // waited for the device to go idle): destroy it, its semaphores and the capture buffer now
    DestroySwapchainLocked();
    if (result != VK_SUCCESS)
    {
        if (result == VK_ERROR_SURFACE_LOST_KHR)
        {
            return SwapchainResult::SurfaceLost;
        }
        error = "vkCreateSwapchainKHR failed (result " + std::to_string(result) + ")";
        return SwapchainResult::Retry;
    }
    state.swapchain = swapchain;

    uint32_t count = 0;
    result = vkGetSwapchainImagesKHR(_device, swapchain, &count, nullptr);
    if (result == VK_SUCCESS && count > 0)
    {
        state.images.resize(count);
        result = vkGetSwapchainImagesKHR(_device, swapchain, &count, state.images.data());
        state.images.resize(std::min<std::size_t>(count, state.images.size()));
    }
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || state.images.empty())
    {
        error = "vkGetSwapchainImagesKHR failed (result " + std::to_string(result) + ")";
        DestroySwapchainLocked();
        return SwapchainResult::Retry;
    }
    state.renderFinished.assign(state.images.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : state.renderFinished)
    {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(_device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS)
        {
            semaphore = VK_NULL_HANDLE;
            error = "vkCreateSemaphore (render finished) failed";
            DestroySwapchainLocked();
            return SwapchainResult::Retry;
        }
    }
    if (capture && !CreateCaptureBufferLocked(static_cast<VkDeviceSize>(extent.extent.width) * extent.extent.height * 4))
    {
        LogOnce("could not make the present capture buffer (test hook): presented images are not captured");
    }

    state.format = format.format;
    state.presentMode = presentMode;
    state.extent = extent.extent;
    state.framebuffer = framebuffer;
    state.requestedImageCount = imageCount;
    state.recreatePending = false;
    state.minimised = false;
    ++state.createCount;
    return SwapchainResult::Created;
}

void VulkanRenderer::DestroySwapchainLocked()
{
    SwapchainState& state = *_swapchain;
    if (_device != VK_NULL_HANDLE)
    {
        for (const VkSemaphore semaphore : state.renderFinished)
        {
            if (semaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(_device, semaphore, nullptr);
            }
        }
        if (state.swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(_device, state.swapchain, nullptr);
        }
        if (state.capture.mapped != nullptr)
        {
            vkUnmapMemory(_device, state.capture.memory);
        }
        if (state.capture.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(_device, state.capture.buffer, nullptr);
        }
        if (state.capture.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(_device, state.capture.memory, nullptr);
        }
    }
    state.renderFinished.clear();
    state.images.clear();
    state.swapchain = VK_NULL_HANDLE;
    state.capture = {};
    state.captureValid = false;
}

void VulkanRenderer::StopPresentingLocked(const std::string& reason)
{
    vkDeviceWaitIdle(_device);
    DestroySwapchainLocked();
    _swapchain->presentingStopped = true;
    std::cerr << "Vulkan renderer: " << reason
              << "; nothing is presented any more (frames still render offscreen)" << std::endl;
}

void VulkanRenderer::RecreateSwapchainLocked()
{
    // Nothing may still use the old swapchain, its images or its semaphores (see the top of this file)
    vkDeviceWaitIdle(_device);
    std::string error;
    switch (CreateSwapchainLocked(error))
    {
    case SwapchainResult::Created:
    case SwapchainResult::Minimised:
        return;
    case SwapchainResult::Retry:
        _swapchain->recreatePending = true;
        LogOnce("could not recreate the swapchain (" + error + "); trying again at the next Present");
        return;
    case SwapchainResult::Unusable:
        StopPresentingLocked(error);
        return;
    case SwapchainResult::SurfaceLost:
        RecreateSurfaceLocked();
        return;
    }
}

void VulkanRenderer::RecreateSurfaceLocked()
{
    std::cerr << "Vulkan renderer: the window's surface was lost; making it again" << std::endl;
    vkDeviceWaitIdle(_device);
    DestroySwapchainLocked();
    DestroySurfaceLocked(); // GLFW allows one surface per window at a time

    // A failure here is not an initialisation failure: keep the status Initialize left
    const InitStatus statusBefore = _initStatus;
    const std::string messageBefore = _initMessage;
    const bool created = CreateSurfaceLocked();
    _initStatus = statusBefore;
    _initMessage = messageBefore;
    if (!created)
    {
        StopPresentingLocked("the window's surface was lost and could not be made again");
        return;
    }
    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(_physicalDevice, _graphicsFamily, _surface, &supported) != VK_SUCCESS ||
        supported != VK_TRUE)
    {
        StopPresentingLocked("the device cannot present to the window's new surface");
        return;
    }
    std::string error;
    switch (CreateSwapchainLocked(error))
    {
    case SwapchainResult::Created:
    case SwapchainResult::Minimised:
        return;
    case SwapchainResult::Retry:
        _swapchain->recreatePending = true;
        LogOnce("could not recreate the swapchain (" + error + "); trying again at the next Present");
        return;
    case SwapchainResult::Unusable:
        StopPresentingLocked(error);
        return;
    case SwapchainResult::SurfaceLost:
        StopPresentingLocked("the window's new surface was lost too");
        return;
    }
}

void VulkanRenderer::MarkDeviceLostLocked(const char* where, VkResult result)
{
    _deviceLost = true;
    std::cerr << "Vulkan renderer: " << where << " failed (result " << result
              << (result == VK_ERROR_DEVICE_LOST ? ", the device was lost" : "") << "); frames are skipped" << std::endl;
}

// ---------------------------------------------------------------------------------------------------------------------
// Present
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::Present()
{
    std::lock_guard lock(_mutex);
    if (!_initialized || _surface == VK_NULL_HANDLE)
    {
        return; // no window: nothing to present (silently, as the software renderer's headless Present)
    }
    SwapchainState& state = *_swapchain;
    const bool frameToPresent = state.frameToPresent;
    state.frameToPresent = false;
    if (!frameToPresent || _deviceLost || state.presentingStopped || _color.image == VK_NULL_HANDLE)
    {
        return;
    }

    // Minimised (0x0): nothing; a new size, a pending recreate or no swapchain yet: recreate first
    const VkExtent2D framebuffer = FramebufferSizeLocked();
    switch (DecideBeforeAcquire(state.swapchain != VK_NULL_HANDLE, state.recreatePending, framebuffer, state.framebuffer))
    {
    case SwapchainAction::Skip:
        return;
    case SwapchainAction::RecreateNow:
        RecreateSwapchainLocked();
        if (_deviceLost || state.presentingStopped || state.minimised || state.recreatePending ||
            state.swapchain == VK_NULL_HANDLE)
        {
            return;
        }
        break;
    default:
        break;
    }

    // The slot of the frame being presented (the last one EndFrame submitted): its previous blit must have finished
    const uint32_t slotIndex = (_currentFrame + MAX_FRAMES_IN_FLIGHT - 1) % MAX_FRAMES_IN_FLIGHT;
    PresentSlot& slot = state.slots[slotIndex];
    VkResult result = vkWaitForFences(_device, 1, &slot.fence, VK_TRUE, PresentTimeoutNanoseconds);
    if (result != VK_SUCCESS)
    {
        MarkDeviceLostLocked("waiting for the previous present", result);
        return;
    }

    uint32_t imageIndex = 0;
    VkResult acquired = VK_SUCCESS;
    const std::optional<VkResult> forcedAcquire = std::exchange(_forcedAcquireResult, std::nullopt);
    if (forcedAcquire && *forcedAcquire < 0)
    {
        acquired = *forcedAcquire; // a test's error: the acquire is not made, so the semaphore stays unsignalled
    }
    else
    {
        acquired = vkAcquireNextImageKHR(_device, state.swapchain, PresentTimeoutNanoseconds, slot.imageAvailable,
                                         VK_NULL_HANDLE, &imageIndex);
        if (forcedAcquire && *forcedAcquire == VK_SUBOPTIMAL_KHR && acquired == VK_SUCCESS)
        {
            acquired = VK_SUBOPTIMAL_KHR; // a test's SUBOPTIMAL: the image was acquired
        }
    }
    switch (DecideAfterAcquire(acquired))
    {
    case SwapchainAction::Continue:
        break;
    case SwapchainAction::RecreateNext:
        state.recreatePending = true; // present this image, recreate before the next
        break;
    case SwapchainAction::RecreateNow:
        state.recreatePending = true; // OUT_OF_DATE: no image; this frame is not presented
        RecreateSwapchainLocked();
        return;
    case SwapchainAction::DeviceLost:
        MarkDeviceLostLocked("vkAcquireNextImageKHR", acquired);
        return;
    case SwapchainAction::SurfaceLost:
        RecreateSurfaceLocked();
        return;
    case SwapchainAction::Skip:
    default:
        LogOnce("vkAcquireNextImageKHR returned " + std::to_string(acquired) + ": that frame was not presented");
        return;
    }
    if (imageIndex >= state.images.size())
    {
        MarkDeviceLostLocked("vkAcquireNextImageKHR (an image index out of range)", acquired);
        return;
    }

    // Record the blit: the whole offscreen image to the whole swapchain image (stretched, no letterbox)
    const VkImage target = state.images[imageIndex];
    const VkCommandBuffer commandBuffer = slot.commandBuffer;
    VkFormatProperties colorProperties{};
    vkGetPhysicalDeviceFormatProperties(_physicalDevice, ColorFormat, &colorProperties);
    const VkFilter filter =
        ChooseBlitFilter({_targetWidth, _targetHeight}, state.extent,
                         (colorProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0);
    const bool capture = state.capture.mapped != nullptr;

    vkResetCommandBuffer(commandBuffer, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        MarkDeviceLostLocked("vkBeginCommandBuffer (present)", VK_ERROR_UNKNOWN);
        return;
    }
    {
        // The frame's colour writes are visible to the blit (the render pass's out-dependency already makes them so; the
        // image stays in TRANSFER_SRC_OPTIMAL), and the swapchain image becomes a transfer destination after the acquire's
        // semaphore wait (which is at the TRANSFER stage, the source stage here)
        const std::array<VkImageMemoryBarrier, 2> barriers = {
            ImageBarrier(_color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            ImageBarrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        vkCmdPipelineBarrier(commandBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             static_cast<uint32_t>(barriers.size()), barriers.data());

        // Row 0 of the offscreen image is the top of the picture (the viewport flips y), as it is for a swapchain image,
        // so no flip; a different channel order (B8G8R8A8) is converted by the blit
        VkImageBlit region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[1] = {static_cast<int32_t>(_targetWidth), static_cast<int32_t>(_targetHeight), 1};
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.dstOffsets[1] = {static_cast<int32_t>(state.extent.width), static_cast<int32_t>(state.extent.height), 1};
        vkCmdBlitImage(commandBuffer, _color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);

        if (capture)
        {
            // Test hook: copy what was written to host memory while the image is still this queue's
            const VkImageMemoryBarrier toSource =
                ImageBarrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toSource);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {state.extent.width, state.extent.height, 1};
            vkCmdCopyImageToBuffer(commandBuffer, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, state.capture.buffer, 1,
                                   &copy);
            VkBufferMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = state.capture.buffer;
            toHost.offset = 0;
            toHost.size = VK_WHOLE_SIZE;
            const VkImageMemoryBarrier toPresent = ImageBarrier(
                target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
                                 &toHost, 1, &toPresent);
        }
        else
        {
            const VkImageMemoryBarrier toPresent = ImageBarrier(
                target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toPresent);
        }
    }
    if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
    {
        MarkDeviceLostLocked("vkEndCommandBuffer (present)", VK_ERROR_UNKNOWN);
        return;
    }

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &slot.imageAvailable;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &state.renderFinished[imageIndex];
    result = vkResetFences(_device, 1, &slot.fence);
    if (result == VK_SUCCESS)
    {
        result = vkQueueSubmit(_graphicsQueue, 1, &submitInfo, slot.fence);
    }
    if (result != VK_SUCCESS)
    {
        MarkDeviceLostLocked("vkQueueSubmit (present)", result);
        return;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &state.renderFinished[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &state.swapchain;
    presentInfo.pImageIndices = &imageIndex;
    VkResult presented = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
    ++state.presentCount;
    if (capture)
    {
        state.captureValid = true;
        state.captureExtent = state.extent;
        state.captureFormat = state.format.format;
    }
    if (const std::optional<VkResult> forced = std::exchange(_forcedPresentResult, std::nullopt);
        forced && presented == VK_SUCCESS)
    {
        presented = *forced; // a test's result: the present was made
    }

    switch (DecideAfterPresent(presented))
    {
    case SwapchainAction::RecreateNext:
        state.recreatePending = true;
        break;
    case SwapchainAction::DeviceLost:
        MarkDeviceLostLocked("vkQueuePresentKHR", presented);
        break;
    case SwapchainAction::SurfaceLost:
        RecreateSurfaceLocked();
        break;
    default:
        if (presented < 0)
        {
            LogOnce("vkQueuePresentKHR returned " + std::to_string(presented) + ": that frame was not shown");
        }
        break;
    }
}
