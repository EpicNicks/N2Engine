#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

// The Vulkan backend's choice of physical device (stage F of #43) as a PURE function over plain descriptions of the
// candidates: no device calls. The renderer (PickPhysicalDevice) fills one DeviceCandidate per enumerated device and
// hands them here; tests/renderer/vulkan/VulkanDeviceChoiceTests.cpp checks the rules with fake candidates on every leg
// that builds the backend.

namespace Renderer::Vulkan
{
    /// What the renderer found out about one physical device
    struct DeviceCandidate
    {
        uint32_t index = 0; // the position in vkEnumeratePhysicalDevices' order
        std::string name;
        VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
        uint32_t apiVersion = 0;
        uint64_t deviceLocalBytes = 0; // the sum of the DEVICE_LOCAL memory heaps
        bool hasGraphics = false;      // a queue family with VK_QUEUE_GRAPHICS_BIT
        bool formatsOk = false;        // the colour target format's features and a depth format
        /// With a surface: one queue family both draws and presents to it, and the surface offers at least one format
        /// and one present mode on this device. Not looked at without a surface
        bool presentOk = false;
        /// With a surface: the device offers VK_KHR_swapchain. Not looked at without a surface
        bool swapchainExtOk = false;
    };

    /// A wish for a particular device: none, an index in enumeration order, or a case-insensitive part of the name
    struct GpuPreference
    {
        enum class Kind
        {
            None,
            Index,
            Name,
        };
        Kind kind = Kind::None;
        uint32_t index = 0;
        std::string name;
    };

    /// The text of N2_VULKAN_GPU or SetGpuPreference: surrounding spaces are ignored; empty is no preference; only digits
    /// is an index (one too large for 32 bits is UINT32_MAX, which names no device); anything else is a name part
    [[nodiscard]] GpuPreference ParseGpuPreference(std::string_view text);

    /// "index 1" or "name 'nvidia'" (for log lines); "none"
    [[nodiscard]] std::string DescribeGpuPreference(const GpuPreference& preference);

    /// Discrete 1000, integrated 500, virtual 200, CPU 100, anything else 10
    [[nodiscard]] uint32_t ScoreDeviceType(VkPhysicalDeviceType type);

    /// "discrete GPU", "integrated GPU", "virtual GPU", "CPU", "other"
    [[nodiscard]] const char* DeviceTypeName(VkPhysicalDeviceType type);

    /// Whether the renderer can use the device: Vulkan 1.1, a graphics queue and the target formats; with a surface
    /// (needsPresent) also presentOk and swapchainExtOk
    [[nodiscard]] bool Qualifies(const DeviceCandidate& candidate, bool needsPresent);

    /// Why Qualifies is false (the first reason), or empty
    [[nodiscard]] std::string WhyNotQualified(const DeviceCandidate& candidate, bool needsPresent);

    /// The default order: true when `a` is preferred to `b`: a higher ScoreDeviceType, then more device-local memory, then
    /// the earlier enumeration index
    [[nodiscard]] bool IsPreferredDevice(const DeviceCandidate& a, const DeviceCandidate& b);

    struct DeviceChoice
    {
        bool found = false;       // false: no candidate qualifies
        std::size_t position = 0; // into the candidates vector
        bool byPreference = false;
        /// Set when a preference was given and not followed (it named nothing, or a device that does not qualify): names
        /// the preference, why, and the device used instead. Empty otherwise
        std::string warning;
    };

    /// The device to use. A preference that names a qualifying device wins (a name part matching several: the default
    /// order among them); any other preference falls back to the default with `warning` set, never a failure. The
    /// default is the best qualifying device by IsPreferredDevice.
    [[nodiscard]] DeviceChoice ChooseDevice(const std::vector<DeviceCandidate>& candidates, bool needsPresent,
                                            const GpuPreference& preference);
}
