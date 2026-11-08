#pragma once

// PRIVATE to renderer/src/vulkan/: the window side of the Vulkan backend (stage F of #43): the swapchain, the per-slot
// present resources and the bookkeeping of recreation. Everything here is made only when the renderer has a surface
// (a window, or the test-only headless surface); a headless renderer leaves it empty.

#include <array>
#include <cstdint>
#include <vector>

#include "renderer/vulkan/VulkanRenderer.hpp"

namespace Renderer::Vulkan
{
    /// The resources of one frame slot's present: the blit's command buffer, the semaphore the acquire signals and the
    /// blit waits on, and the fence of the blit's submission. A slot's present waits for that fence before it records or
    /// acquires again, so neither its command buffer nor its semaphore is reused while pending.
    struct PresentSlot
    {
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE; // from the renderer's command pool (freed with it)
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE; // created signalled
    };

    struct SwapchainState
    {
        std::array<PresentSlot, 2> slots{}; // one per frame in flight (VulkanRenderer::MAX_FRAMES_IN_FLIGHT)

        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> images;
        /// One per swapchain image: the blit signals the one of the image it wrote and the present waits on it. Per image,
        /// not per frame slot, because a semaphore a present waits on can only be signalled again once that image has come
        /// back through an acquire
        std::vector<VkSemaphore> renderFinished;
        VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D extent{0, 0};
        VkExtent2D framebuffer{0, 0}; // the framebuffer size the swapchain was made for
        uint32_t requestedImageCount = 0;

        bool recreatePending = false; // a resize, a vsync change or a SUBOPTIMAL / OUT_OF_DATE result
        bool minimised = false;       // the last recreate found a 0 extent: no swapchain change, nothing presented
        bool presentingStopped = false; // the swapchain cannot be made (no usable format or usage, a lost surface)
        bool frameToPresent = false;  // a frame was submitted since the last Present

        // Counters for the tests
        uint32_t createCount = 0;
        uint32_t presentCount = 0;
        uint32_t surfaceCreateCount = 0;

        // Test only: a host copy of each presented swapchain image (SetPresentCaptureForTest)
        VulkanBuffer capture;
        VkExtent2D captureExtent{0, 0};
        VkFormat captureFormat = VK_FORMAT_UNDEFINED;
        bool captureValid = false;
    };
}
