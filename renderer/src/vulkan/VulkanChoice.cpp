#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "renderer/vulkan/VulkanDeviceChoice.hpp"
#include "renderer/vulkan/VulkanSwapchainChoice.hpp"

// The pure choice functions of stage F (#43): the swapchain's format, present mode, extent, image count, composite alpha
// and blit filter, the swapchain decision table, and the device choice. Nothing here calls Vulkan.

namespace Renderer::Vulkan
{
    // -----------------------------------------------------------------------------------------------------------------
    // Swapchain
    // -----------------------------------------------------------------------------------------------------------------

    bool IsSrgbFormat(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
        }
    }

    SurfaceFormatChoice ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
    {
        SurfaceFormatChoice choice;
        if (formats.empty())
        {
            return choice;
        }
        // One UNDEFINED entry: the surface takes any format
        if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
        {
            choice.found = true;
            choice.format = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
            return choice;
        }

        // UNORM formats take the offscreen image's encoded bytes as they are, in this order of preference
        constexpr VkFormat unormPreference[] = {
            VK_FORMAT_B8G8R8A8_UNORM,           VK_FORMAT_R8G8B8A8_UNORM,           VK_FORMAT_A8B8G8R8_UNORM_PACK32,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32,
        };
        for (const VkFormat wanted : unormPreference)
        {
            for (const VkSurfaceFormatKHR& offered : formats)
            {
                if (offered.format == wanted && offered.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                {
                    choice.found = true;
                    choice.format = offered;
                    return choice;
                }
            }
        }

        // Only sRGB formats: the first one, knowing the blit will encode the encoded bytes again
        for (const VkSurfaceFormatKHR& offered : formats)
        {
            if (IsSrgbFormat(offered.format) && offered.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            {
                choice.found = true;
                choice.format = offered;
                choice.doubleEncoded = true;
                return choice;
            }
        }
        return choice;
    }

    VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync)
    {
        if (vsync)
        {
            return VK_PRESENT_MODE_FIFO_KHR;
        }
        const auto offered = [&modes](VkPresentModeKHR mode)
        { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
        if (offered(VK_PRESENT_MODE_MAILBOX_KHR))
        {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
        if (offered(VK_PRESENT_MODE_IMMEDIATE_KHR))
        {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    bool IsMinimised(VkExtent2D extent)
    {
        return extent.width == 0 || extent.height == 0;
    }

    ExtentChoice ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t framebufferWidth,
                              uint32_t framebufferHeight)
    {
        ExtentChoice choice;
        if (capabilities.currentExtent.width != kUndefinedSurfaceExtent)
        {
            choice.extent = capabilities.currentExtent;
        }
        else
        {
            // Clamped per axis; never below 1 because the minimum is at least 1, but a 0 framebuffer stays minimised
            const auto clamp = [](uint32_t value, uint32_t low, uint32_t high)
            { return std::min(std::max(value, low), high); };
            choice.extent.width =
                clamp(framebufferWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            choice.extent.height =
                clamp(framebufferHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        choice.minimised = framebufferWidth == 0 || framebufferHeight == 0 || IsMinimised(choice.extent);
        if (choice.minimised)
        {
            choice.extent = {0, 0};
        }
        return choice;
    }

    uint32_t ChooseImageCount(const VkSurfaceCapabilitiesKHR& capabilities)
    {
        uint32_t count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount != 0 && count > capabilities.maxImageCount)
        {
            count = capabilities.maxImageCount;
        }
        return count;
    }

    VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
    {
        constexpr VkCompositeAlphaFlagBitsKHR order[] = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (const VkCompositeAlphaFlagBitsKHR bit : order)
        {
            if ((supported & bit) != 0)
            {
                return bit;
            }
        }
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    VkFilter ChooseBlitFilter(VkExtent2D source, VkExtent2D destination, bool linearFilterSupported)
    {
        const bool sameSize = source.width == destination.width && source.height == destination.height;
        return (!sameSize && linearFilterSupported) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    }

    const char* ToString(SwapchainAction action)
    {
        switch (action)
        {
        case SwapchainAction::Continue: return "Continue";
        case SwapchainAction::RecreateNow: return "RecreateNow";
        case SwapchainAction::RecreateNext: return "RecreateNext";
        case SwapchainAction::Skip: return "Skip";
        case SwapchainAction::DeviceLost: return "DeviceLost";
        case SwapchainAction::SurfaceLost: return "SurfaceLost";
        }
        return "?";
    }

    SwapchainAction DecideBeforeAcquire(bool hasSwapchain, bool recreatePending, VkExtent2D framebuffer,
                                        VkExtent2D swapchainFramebuffer)
    {
        if (IsMinimised(framebuffer))
        {
            return SwapchainAction::Skip;
        }
        if (!hasSwapchain || recreatePending || framebuffer.width != swapchainFramebuffer.width ||
            framebuffer.height != swapchainFramebuffer.height)
        {
            return SwapchainAction::RecreateNow;
        }
        return SwapchainAction::Continue;
    }

    SwapchainAction DecideAfterAcquire(VkResult result)
    {
        switch (result)
        {
        case VK_SUCCESS: return SwapchainAction::Continue;
        case VK_SUBOPTIMAL_KHR: return SwapchainAction::RecreateNext;
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainAction::RecreateNow;
        case VK_ERROR_DEVICE_LOST: return SwapchainAction::DeviceLost;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapchainAction::SurfaceLost;
        default: return SwapchainAction::Skip;
        }
    }

    SwapchainAction DecideAfterPresent(VkResult result)
    {
        switch (result)
        {
        case VK_SUBOPTIMAL_KHR:
        case VK_ERROR_OUT_OF_DATE_KHR: return SwapchainAction::RecreateNext;
        case VK_ERROR_DEVICE_LOST: return SwapchainAction::DeviceLost;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapchainAction::SurfaceLost;
        default: return SwapchainAction::Continue;
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Device
    // -----------------------------------------------------------------------------------------------------------------

    namespace
    {
        std::string Lowercase(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        bool NameMatches(const std::string& name, const std::string& part)
        {
            return Lowercase(name).find(Lowercase(part)) != std::string::npos;
        }
    }

    GpuPreference ParseGpuPreference(std::string_view text)
    {
        const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
        while (!text.empty() && isSpace(text.front()))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && isSpace(text.back()))
        {
            text.remove_suffix(1);
        }

        GpuPreference preference;
        if (text.empty())
        {
            return preference;
        }
        const bool digits =
            std::all_of(text.begin(), text.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        if (digits)
        {
            preference.kind = GpuPreference::Kind::Index;
            uint64_t value = 0;
            for (const char c : text)
            {
                value = value * 10 + static_cast<uint64_t>(c - '0');
                if (value > std::numeric_limits<uint32_t>::max())
                {
                    value = std::numeric_limits<uint32_t>::max();
                    break;
                }
            }
            preference.index = static_cast<uint32_t>(value);
            return preference;
        }
        preference.kind = GpuPreference::Kind::Name;
        preference.name = std::string(text);
        return preference;
    }

    std::string DescribeGpuPreference(const GpuPreference& preference)
    {
        switch (preference.kind)
        {
        case GpuPreference::Kind::Index: return "index " + std::to_string(preference.index);
        case GpuPreference::Kind::Name: return "name '" + preference.name + "'";
        case GpuPreference::Kind::None: break;
        }
        return "none";
    }

    uint32_t ScoreDeviceType(VkPhysicalDeviceType type)
    {
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1000;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 500;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 200;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 100;
        default: return 10;
        }
    }

    const char* DeviceTypeName(VkPhysicalDeviceType type)
    {
        switch (type)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "CPU";
        default: return "other";
        }
    }

    std::string WhyNotQualified(const DeviceCandidate& candidate, bool needsPresent)
    {
        if (candidate.apiVersion < VK_API_VERSION_1_1)
        {
            return "it does not support Vulkan 1.1";
        }
        if (!candidate.hasGraphics)
        {
            return "it has no graphics queue";
        }
        if (!candidate.formatsOk)
        {
            return "it lacks the colour or depth format the render target needs";
        }
        if (needsPresent && !candidate.swapchainExtOk)
        {
            return "it does not offer VK_KHR_swapchain";
        }
        if (needsPresent && !candidate.presentOk)
        {
            return "no queue family of it both draws and presents to the window, or the window's surface offers it no "
                   "format or present mode";
        }
        return {};
    }

    bool Qualifies(const DeviceCandidate& candidate, bool needsPresent)
    {
        return WhyNotQualified(candidate, needsPresent).empty();
    }

    bool IsPreferredDevice(const DeviceCandidate& a, const DeviceCandidate& b)
    {
        const uint32_t scoreA = ScoreDeviceType(a.type);
        const uint32_t scoreB = ScoreDeviceType(b.type);
        if (scoreA != scoreB)
        {
            return scoreA > scoreB;
        }
        if (a.deviceLocalBytes != b.deviceLocalBytes)
        {
            return a.deviceLocalBytes > b.deviceLocalBytes;
        }
        return a.index < b.index;
    }

    DeviceChoice ChooseDevice(const std::vector<DeviceCandidate>& candidates, bool needsPresent,
                              const GpuPreference& preference)
    {
        // The best qualifying candidate among those `accept` lets through
        const auto best = [&](const auto& accept) -> std::ptrdiff_t
        {
            std::ptrdiff_t chosen = -1;
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                if (!Qualifies(candidates[i], needsPresent) || !accept(candidates[i]))
                {
                    continue;
                }
                if (chosen < 0 || IsPreferredDevice(candidates[i], candidates[static_cast<std::size_t>(chosen)]))
                {
                    chosen = static_cast<std::ptrdiff_t>(i);
                }
            }
            return chosen;
        };

        DeviceChoice choice;
        const std::ptrdiff_t fallback = best([](const DeviceCandidate&) { return true; });
        if (fallback < 0)
        {
            return choice; // nothing qualifies, preference or not
        }
        choice.found = true;
        choice.position = static_cast<std::size_t>(fallback);
        if (preference.kind == GpuPreference::Kind::None)
        {
            return choice;
        }

        const auto named = [&preference](const DeviceCandidate& candidate)
        {
            return preference.kind == GpuPreference::Kind::Index ? candidate.index == preference.index
                                                                 : NameMatches(candidate.name, preference.name);
        };
        const std::ptrdiff_t preferred = best(named);
        if (preferred >= 0)
        {
            choice.position = static_cast<std::size_t>(preferred);
            choice.byPreference = true;
            return choice;
        }

        // Not followed: say what the preference named, why it cannot be used, and what is used instead
        const std::string instead = " using '" + candidates[static_cast<std::size_t>(fallback)].name + "' instead";
        const DeviceCandidate* first = nullptr;
        for (const DeviceCandidate& candidate : candidates)
        {
            if (named(candidate))
            {
                first = &candidate;
                break;
            }
        }
        if (first == nullptr)
        {
            choice.warning = "the GPU preference " + DescribeGpuPreference(preference) + " names no Vulkan device (" +
                std::to_string(candidates.size()) + " found);" + instead;
        }
        else
        {
            choice.warning = "the GPU preference " + DescribeGpuPreference(preference) + " names '" + first->name +
                "', which cannot be used (" + WhyNotQualified(*first, needsPresent) + ");" + instead;
        }
        return choice;
    }
}
