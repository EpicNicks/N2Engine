#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>

#include "renderer/vulkan/VulkanRenderer.hpp"

// Stage 0 of the Vulkan backend (#43): headless initialisation, an offscreen colour and depth target, a frame that
// clears it, readback, resizing and shutdown. Everything that draws is still a stub that fails loudly and cleanly (logs
// once, returns nullptr/false). Nothing here needs a window, a surface or a swapchain.
//
// Object lifetimes, in creation order (Shutdown destroys them in reverse):
//   instance -> debug messenger -> device -> command pool (+ the frames' command buffers and fences) -> render pass ->
//   targets (colour and depth images + views, framebuffer, host-visible readback buffer)
//
// Frames in flight: two FrameResources (a command buffer and a fence each) alternate. BeginFrame waits for the fence of
// its slot, EndFrame resets it and submits with it, so the CPU runs at most two frames ahead. Both frames render into
// the same colour image; the render pass's external dependencies order a frame after the previous one (and after any
// readback copy) and make its writes visible to the copy that reads them.
//
// Image layouts: the colour image is UNDEFINED, then TRANSFER_DST for one initial clear to black, then rests in
// TRANSFER_SRC_OPTIMAL (the render pass's initial layout is UNDEFINED, it clears, and its final layout is
// TRANSFER_SRC_OPTIMAL), which is what the readback copy (and stage F's blit to a swapchain) reads from. The depth image
// is only ever used inside the render pass (UNDEFINED in, DEPTH_STENCIL_ATTACHMENT_OPTIMAL out, contents not stored).

using namespace Renderer::Vulkan;

namespace Renderer::Vulkan
{
    /// The messenger's counters, kept in a block the callback reaches through a raw pointer; the renderer owns it
    /// through a shared_ptr that outlives the messenger
    struct DebugState
    {
        std::mutex mutex;
        ValidationStats stats;
        std::vector<std::string> messages;
    };
}

namespace
{
    constexpr const char* ValidationLayerName = "VK_LAYER_KHRONOS_validation";
    constexpr std::size_t MaxStoredMessages = 32;
    constexpr uint64_t FenceTimeoutNanoseconds = 10ull * 1000ull * 1000ull * 1000ull;

    std::string GetEnvironment(const char* name)
    {
        std::string value;
#ifdef _MSC_VER
        // _dupenv_s, not getenv (which MSVC deprecates); an unset variable succeeds with a null buffer
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

    VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT types,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
    {
        const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
        const bool isWarning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
        const bool isValidationType = (types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) != 0;
        const std::string text = (data != nullptr && data->pMessage != nullptr) ? data->pMessage : "(no message)";

        if (auto* state = static_cast<DebugState*>(userData); state != nullptr)
        {
            std::lock_guard lock(state->mutex);
            if (isError)
            {
                ++state->stats.errors;
            }
            else if (isWarning)
            {
                if (isValidationType)
                {
                    ++state->stats.warnings;
                }
                else
                {
                    ++state->stats.otherWarnings;
                }
            }
            if ((isError || (isWarning && isValidationType)) && state->messages.size() < MaxStoredMessages)
            {
                state->messages.push_back(text);
            }
        }

        std::cerr << "Vulkan " << (isError ? "error: " : "warning: ") << text << std::endl;
        return VK_FALSE; // never abort the call that triggered the message
    }

    void FillMessengerInfo(VkDebugUtilsMessengerCreateInfoEXT& info, void* userData)
    {
        info = {};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = DebugCallback;
        info.pUserData = userData;
    }

    /// The first of `candidates` whose optimal-tiling features include all of `features`, or VK_FORMAT_UNDEFINED
    VkFormat FindSupportedFormat(VkPhysicalDevice device, const std::vector<VkFormat>& candidates,
                                 VkFormatFeatureFlags features)
    {
        for (const VkFormat format : candidates)
        {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(device, format, &properties);
            if ((properties.optimalTilingFeatures & features) == features)
            {
                return format;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    VkFormat FindDepthFormat(VkPhysicalDevice device)
    {
        return FindSupportedFormat(
            device, {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT},
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
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

    VkImageMemoryBarrier ColorBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
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
}

// ---------------------------------------------------------------------------------------------------------------------
// Construction and the test/diagnostic accessors
// ---------------------------------------------------------------------------------------------------------------------

VulkanRenderer::VulkanRenderer() : _debug(std::make_shared<DebugState>())
{
}

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

void VulkanRenderer::SetValidationMode(ValidationMode mode)
{
    std::lock_guard lock(_mutex);
    _validationMode = mode;
}

InitStatus VulkanRenderer::GetInitStatus() const
{
    std::lock_guard lock(_mutex);
    return _initStatus;
}

std::string VulkanRenderer::GetInitMessage() const
{
    std::lock_guard lock(_mutex);
    return _initMessage;
}

bool VulkanRenderer::IsValidationEnabled() const
{
    std::lock_guard lock(_mutex);
    return _validationEnabled;
}

ValidationStats VulkanRenderer::GetValidationStats() const
{
    std::lock_guard lock(_debug->mutex);
    return _debug->stats;
}

std::vector<std::string> VulkanRenderer::GetValidationMessages() const
{
    std::lock_guard lock(_debug->mutex);
    return _debug->messages;
}

void VulkanRenderer::ResetValidationStats()
{
    std::lock_guard lock(_debug->mutex);
    _debug->stats = {};
    _debug->messages.clear();
}

bool VulkanRenderer::SubmitDebugMessageForTest(bool asError, const std::string& text)
{
    PFN_vkSubmitDebugUtilsMessageEXT submit = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    {
        std::lock_guard lock(_mutex);
        if (_instance == VK_NULL_HANDLE || !_validationEnabled || _debugMessenger == VK_NULL_HANDLE)
        {
            return false;
        }
        instance = _instance;
        submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
            vkGetInstanceProcAddr(instance, "vkSubmitDebugUtilsMessageEXT"));
    }
    if (submit == nullptr)
    {
        return false;
    }
    VkDebugUtilsMessengerCallbackDataEXT data{};
    data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
    data.pMessageIdName = "N2EngineTestMessage";
    data.pMessage = text.c_str();
    // The same messenger (and so the same callback and counters) the validation layer reports through
    submit(instance, asError ? VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT : VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
    return true;
}

std::string VulkanRenderer::GetDeviceName() const
{
    std::lock_guard lock(_mutex);
    return _physicalDevice != VK_NULL_HANDLE ? std::string(_deviceProperties.deviceName) : std::string();
}

VkPhysicalDeviceType VulkanRenderer::GetDeviceType() const
{
    std::lock_guard lock(_mutex);
    return _physicalDevice != VK_NULL_HANDLE ? _deviceProperties.deviceType : VK_PHYSICAL_DEVICE_TYPE_OTHER;
}

uint32_t VulkanRenderer::GetRenderWidth() const
{
    std::lock_guard lock(_mutex);
    return _targetWidth;
}

uint32_t VulkanRenderer::GetRenderHeight() const
{
    std::lock_guard lock(_mutex);
    return _targetHeight;
}

void VulkanRenderer::LogUnsupportedOnce(const char* what) const
{
    {
        std::lock_guard lock(_logMutex);
        if (!_loggedUnsupported.insert(what).second)
        {
            return;
        }
    }
    std::cerr << "Vulkan renderer: " << what
              << " is not implemented yet (#43, stages A-E); it does nothing and returns null/false" << std::endl;
}

// ---------------------------------------------------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------------------------------------------------

bool VulkanRenderer::Fail(InitStatus status, const std::string& message)
{
    _initStatus = status;
    _initMessage = message;
    std::cerr << "Vulkan renderer: " << message << std::endl;
    return false;
}

bool VulkanRenderer::WantsValidation(bool& required) const
{
    required = false;
    switch (_validationMode)
    {
    case ValidationMode::Enabled:
        required = true;
        return true;
    case ValidationMode::Disabled:
        return false;
    case ValidationMode::Default:
        break;
    }

    const std::string variable = GetEnvironment("N2_VULKAN_VALIDATION");
    if (variable == "1")
    {
        required = true;
        return true;
    }
    if (variable == "0")
    {
        return false;
    }
#ifdef NDEBUG
    return false; // Release: off unless asked for
#else
    return true; // Debug: on when the layer is installed
#endif
}

bool VulkanRenderer::Initialize(GLFWwindow* windowHandle, uint32_t width, uint32_t height)
{
    std::lock_guard lock(_mutex);

    if (_instance != VK_NULL_HANDLE)
    {
        // Initialized again without a Shutdown: a new renderer for everything holding the old one's token
        ShutdownLocked();
        EndLifetime();
    }
    _initStatus = InitStatus::NotInitialized;
    _initMessage.clear();
    _warnedWindow = false;
    _requestedWidth = width;
    _requestedHeight = height;
    _targetFixed = false;
    static_cast<void>(windowHandle); // no surface yet: frames render offscreen whether or not there is a window

    if (width == 0 || height == 0)
    {
        Fail(InitStatus::Failed, "Initialize needs a non-zero size");
        return false;
    }

    const bool ok = CreateInstance() && SetupDebugMessenger() && PickPhysicalDevice() && CreateLogicalDevice() &&
        CreateCommandPool() && CreateFrameResources() && CreateRenderPass() && CreateTargets(width, height);
    if (!ok)
    {
        // Every step above leaves what it made in the members; destroy it all, keeping the reason
        const InitStatus status = _initStatus;
        const std::string message = _initMessage;
        ShutdownLocked();
        _initStatus = status == InitStatus::NotInitialized ? InitStatus::Failed : status;
        _initMessage = message;
        return false;
    }

    _initialized = true;
    _initStatus = InitStatus::Ok;
    return true;
}

bool VulkanRenderer::CreateInstance()
{
    bool validationRequired = false;
    bool validation = WantsValidation(validationRequired);

    bool layerPresent = false;
    bool debugUtilsPresent = false;
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceLayerProperties(&count, nullptr) == VK_SUCCESS && count > 0)
        {
            std::vector<VkLayerProperties> layers(count);
            const VkResult result = vkEnumerateInstanceLayerProperties(&count, layers.data());
            if (result == VK_SUCCESS || result == VK_INCOMPLETE)
            {
                for (uint32_t i = 0; i < count && i < layers.size(); ++i)
                {
                    if (std::strcmp(layers[i].layerName, ValidationLayerName) == 0)
                    {
                        layerPresent = true;
                    }
                }
            }
        }
    }
    {
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) == VK_SUCCESS && count > 0)
        {
            std::vector<VkExtensionProperties> extensions(count);
            const VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
            if (result == VK_SUCCESS || result == VK_INCOMPLETE)
            {
                for (uint32_t i = 0; i < count && i < extensions.size(); ++i)
                {
                    if (std::strcmp(extensions[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                    {
                        debugUtilsPresent = true;
                    }
                }
            }
        }
    }

    if (validation && !(layerPresent && debugUtilsPresent))
    {
        if (validationRequired)
        {
            return Fail(InitStatus::ValidationUnavailable,
                        std::string("validation was requested but ") +
                            (layerPresent ? "VK_EXT_debug_utils" : ValidationLayerName) + " is not installed");
        }
        std::cerr << "Vulkan renderer: the validation layer is not installed; running without it" << std::endl;
        validation = false;
    }
    _validationEnabled = validation;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "N2Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "N2Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    const char* layers[] = {ValidationLayerName};
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};

    // A messenger in the create info's pNext also reports what happens during vkCreateInstance and vkDestroyInstance
    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
    FillMessengerInfo(messengerInfo, _debug.get());

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    if (validation)
    {
        createInfo.enabledLayerCount = 1;
        createInfo.ppEnabledLayerNames = layers;
        createInfo.enabledExtensionCount = 1;
        createInfo.ppEnabledExtensionNames = extensions;
        createInfo.pNext = &messengerInfo;
    }

    const VkResult result = vkCreateInstance(&createInfo, nullptr, &_instance);
    if (result == VK_SUCCESS)
    {
        return true;
    }
    _instance = VK_NULL_HANDLE;
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
    {
        return Fail(InitStatus::NoVulkanRuntime, "no Vulkan driver is installed (vkCreateInstance: incompatible driver)");
    }
    if (result == VK_ERROR_LAYER_NOT_PRESENT || result == VK_ERROR_EXTENSION_NOT_PRESENT)
    {
        return Fail(InitStatus::ValidationUnavailable,
                    "vkCreateInstance could not load the validation layer (result " + std::to_string(result) + ")");
    }
    return Fail(InitStatus::Failed, "vkCreateInstance failed (result " + std::to_string(result) + ")");
}

bool VulkanRenderer::SetupDebugMessenger()
{
    if (!_validationEnabled)
    {
        return true;
    }

    VkDebugUtilsMessengerCreateInfoEXT info{};
    FillMessengerInfo(info, _debug.get());
    const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));
    if (create == nullptr || create(_instance, &info, nullptr, &_debugMessenger) != VK_SUCCESS)
    {
        _debugMessenger = VK_NULL_HANDLE;
        return Fail(InitStatus::Failed, "could not create the debug messenger");
    }
    return true;
}

bool VulkanRenderer::PickPhysicalDevice()
{
    uint32_t count = 0;
    VkResult result = vkEnumeratePhysicalDevices(_instance, &count, nullptr);
    if (result != VK_SUCCESS)
    {
        return Fail(InitStatus::Failed, "vkEnumeratePhysicalDevices failed (result " + std::to_string(result) + ")");
    }
    if (count == 0)
    {
        return Fail(InitStatus::NoSuitableDevice, "no Vulkan physical device is available");
    }
    std::vector<VkPhysicalDevice> devices(count);
    result = vkEnumeratePhysicalDevices(_instance, &count, devices.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        return Fail(InitStatus::Failed, "vkEnumeratePhysicalDevices failed (result " + std::to_string(result) + ")");
    }
    devices.resize(std::min<std::size_t>(count, devices.size()));

    constexpr VkFormatFeatureFlags colorFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    uint32_t bestScore = 0;
    for (const VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1)
        {
            continue;
        }

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        std::optional<uint32_t> graphicsFamily;
        for (uint32_t i = 0; i < familyCount; ++i)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueCount > 0)
            {
                graphicsFamily = i;
                break;
            }
        }
        if (!graphicsFamily)
        {
            continue;
        }

        VkFormatProperties colorProperties{};
        vkGetPhysicalDeviceFormatProperties(device, ColorFormat, &colorProperties);
        if ((colorProperties.optimalTilingFeatures & colorFeatures) != colorFeatures)
        {
            continue;
        }
        const VkFormat depthFormat = FindDepthFormat(device);
        if (depthFormat == VK_FORMAT_UNDEFINED)
        {
            continue;
        }

        const uint32_t score = ScoreDeviceType(properties.deviceType);
        if (score > bestScore)
        {
            bestScore = score;
            _physicalDevice = device;
            _deviceProperties = properties;
            _graphicsFamily = *graphicsFamily;
            _depthFormat = depthFormat;
        }
    }

    if (_physicalDevice == VK_NULL_HANDLE)
    {
        return Fail(InitStatus::NoSuitableDevice,
                    "no Vulkan device has a graphics queue, Vulkan 1.1 and the colour and depth formats the render "
                    "target needs");
    }
    return true;
}

bool VulkanRenderer::CreateLogicalDevice()
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = _graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // A portability device (MoltenVK) must have VK_KHR_portability_subset enabled when it offers it
    std::vector<const char*> extensions;
    {
        uint32_t count = 0;
        if (vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS && count > 0)
        {
            std::vector<VkExtensionProperties> available(count);
            const VkResult result =
                vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &count, available.data());
            if (result == VK_SUCCESS || result == VK_INCOMPLETE)
            {
                for (uint32_t i = 0; i < count && i < available.size(); ++i)
                {
                    if (std::strcmp(available[i].extensionName, "VK_KHR_portability_subset") == 0)
                    {
                        extensions.push_back("VK_KHR_portability_subset");
                    }
                }
            }
        }
    }

    VkPhysicalDeviceFeatures features{}; // nothing optional is used yet

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

    const VkResult result = vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device);
    if (result != VK_SUCCESS)
    {
        _device = VK_NULL_HANDLE;
        return Fail(InitStatus::Failed, "vkCreateDevice failed (result " + std::to_string(result) + ")");
    }
    vkGetDeviceQueue(_device, _graphicsFamily, 0, &_graphicsQueue);
    return true;
}

bool VulkanRenderer::CreateCommandPool()
{
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = _graphicsFamily;
    const VkResult result = vkCreateCommandPool(_device, &info, nullptr, &_commandPool);
    if (result != VK_SUCCESS)
    {
        _commandPool = VK_NULL_HANDLE;
        return Fail(InitStatus::Failed, "vkCreateCommandPool failed (result " + std::to_string(result) + ")");
    }
    return true;
}

bool VulkanRenderer::CreateFrameResources()
{
    std::array<VkCommandBuffer, MAX_FRAMES_IN_FLIGHT> buffers{};
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = _commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    const VkResult allocated = vkAllocateCommandBuffers(_device, &allocateInfo, buffers.data());
    if (allocated != VK_SUCCESS)
    {
        return Fail(InitStatus::Failed, "vkAllocateCommandBuffers failed (result " + std::to_string(allocated) + ")");
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        _frames[i].commandBuffer = buffers[i];
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // the first BeginFrame of each slot must not wait
        const VkResult result = vkCreateFence(_device, &fenceInfo, nullptr, &_frames[i].fence);
        if (result != VK_SUCCESS)
        {
            _frames[i].fence = VK_NULL_HANDLE;
            return Fail(InitStatus::Failed, "vkCreateFence failed (result " + std::to_string(result) + ")");
        }
    }
    _currentFrame = 0;
    return true;
}

bool VulkanRenderer::CreateRenderPass()
{
    std::array<VkAttachmentDescription, 2> attachments{};
    attachments[0].format = ColorFormat;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; // cleared, so the old contents are not needed
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; // read back, and blitted to a swapchain (stage F)

    attachments[1].format = _depthFormat;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; // only used within the frame
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthReference{};
    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    constexpr VkPipelineStageFlags attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    constexpr VkAccessFlags attachmentWrites =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkSubpassDependency, 2> dependencies{};
    // In: this frame waits for the previous frame's attachment writes and for a readback copy of the last frame (and the
    // initial clear), before its attachments are cleared and written
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = attachmentStages | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].srcAccessMask = attachmentWrites | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstStageMask = attachmentStages;
    dependencies[0].dstAccessMask = attachmentWrites | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    // Out: the colour writes (and the layout change to TRANSFER_SRC) are visible to the transfer that reads the image
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = attachmentStages;
    dependencies[1].srcAccessMask = attachmentWrites;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies = dependencies.data();
    const VkResult result = vkCreateRenderPass(_device, &info, nullptr, &_renderPass);
    if (result != VK_SUCCESS)
    {
        _renderPass = VK_NULL_HANDLE;
        return Fail(InitStatus::Failed, "vkCreateRenderPass failed (result " + std::to_string(result) + ")");
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The offscreen target
// ---------------------------------------------------------------------------------------------------------------------

bool VulkanRenderer::AllocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                                    VkDeviceMemory& memory, VkMemoryPropertyFlags* chosenFlags) const
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[i].propertyFlags;
        if ((requirements.memoryTypeBits & (1u << i)) == 0 || (flags & required) != required)
        {
            continue;
        }
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = i;
        if (vkAllocateMemory(_device, &info, nullptr, &memory) == VK_SUCCESS)
        {
            if (chosenFlags != nullptr)
            {
                *chosenFlags = flags;
            }
            return true;
        }
        memory = VK_NULL_HANDLE; // try the next matching type
    }
    return false;
}

bool VulkanRenderer::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                                 VkImageAspectFlags aspect, VulkanImage& out)
{
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(_device, &info, nullptr, &out.image) != VK_SUCCESS)
    {
        out.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(_device, out.image, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.memory) &&
        !AllocateMemory(requirements, 0, out.memory))
    {
        DestroyImage(out);
        return false;
    }
    if (vkBindImageMemory(_device, out.image, out.memory, 0) != VK_SUCCESS)
    {
        DestroyImage(out);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(_device, &viewInfo, nullptr, &out.view) != VK_SUCCESS)
    {
        out.view = VK_NULL_HANDLE;
        DestroyImage(out);
        return false;
    }
    return true;
}

void VulkanRenderer::DestroyImage(VulkanImage& image)
{
    if (image.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(_device, image.view, nullptr);
    }
    if (image.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(_device, image.image, nullptr);
    }
    if (image.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(_device, image.memory, nullptr);
    }
    image = {};
}

bool VulkanRenderer::CreateReadbackBuffer(VkDeviceSize size)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_device, &info, nullptr, &_readback.buffer) != VK_SUCCESS)
    {
        _readback.buffer = VK_NULL_HANDLE;
        return false;
    }
    _readback.size = size;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(_device, _readback.buffer, &requirements);
    // Cached host memory reads fastest; coherent memory needs no invalidate. Take the best the device offers
    constexpr VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    constexpr VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkMemoryPropertyFlags chosen = 0;
    const bool allocated = AllocateMemory(requirements, visible | coherent | cached, _readback.memory, &chosen) ||
        AllocateMemory(requirements, visible | cached, _readback.memory, &chosen) ||
        AllocateMemory(requirements, visible | coherent, _readback.memory, &chosen) ||
        AllocateMemory(requirements, visible, _readback.memory, &chosen);
    if (!allocated)
    {
        DestroyReadbackBuffer();
        return false;
    }
    _readback.coherent = (chosen & coherent) != 0;

    if (vkBindBufferMemory(_device, _readback.buffer, _readback.memory, 0) != VK_SUCCESS ||
        vkMapMemory(_device, _readback.memory, 0, VK_WHOLE_SIZE, 0, &_readback.mapped) != VK_SUCCESS)
    {
        _readback.mapped = nullptr;
        DestroyReadbackBuffer();
        return false;
    }
    return true;
}

void VulkanRenderer::DestroyReadbackBuffer()
{
    if (_readback.mapped != nullptr)
    {
        vkUnmapMemory(_device, _readback.memory);
    }
    if (_readback.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(_device, _readback.buffer, nullptr);
    }
    if (_readback.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(_device, _readback.memory, nullptr);
    }
    _readback = {};
}

template <typename Recorder>
bool VulkanRenderer::SubmitOneTime(Recorder&& record) const
{
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = _commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(_device, &allocateInfo, &commandBuffer) != VK_SUCCESS)
    {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool ok = vkBeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS;
    if (ok)
    {
        record(commandBuffer);
        ok = vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
    }
    if (ok)
    {
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        ok = vkQueueSubmit(_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
    }
    if (ok)
    {
        // Idle, not a fence: this also waits for every frame submitted before it, which is what a readback needs
        ok = vkQueueWaitIdle(_graphicsQueue) == VK_SUCCESS;
    }
    vkFreeCommandBuffers(_device, _commandPool, 1, &commandBuffer);
    return ok;
}

bool VulkanRenderer::ClearTargetToBlack()
{
    const VkImage image = _color.image;
    return SubmitOneTime(
        [image](VkCommandBuffer commandBuffer)
        {
            VkImageMemoryBarrier toTransferDst = ColorBarrier(
                image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toTransferDst);

            const VkClearColorValue black{};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            vkCmdClearColorImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

            VkImageMemoryBarrier toTransferSrc =
                ColorBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &toTransferSrc);
        });
}

bool VulkanRenderer::CreateTargets(uint32_t width, uint32_t height)
{
    const VkPhysicalDeviceLimits& limits = _deviceProperties.limits;
    const uint32_t maxDimension = std::min({limits.maxImageDimension2D, limits.maxFramebufferWidth});
    const uint32_t maxHeight = std::min({limits.maxImageDimension2D, limits.maxFramebufferHeight});
    if (width == 0 || height == 0 || width > maxDimension || height > maxHeight)
    {
        return Fail(InitStatus::Failed, "a render target of " + std::to_string(width) + "x" + std::to_string(height) +
                                            " is outside what the device supports (at most " +
                                            std::to_string(maxDimension) + "x" + std::to_string(maxHeight) + ")");
    }

    const bool ok = [&]
    {
        if (!CreateImage(width, height, ColorFormat,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, _color))
        {
            return false;
        }
        if (!CreateImage(width, height, _depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT, _depth))
        {
            return false;
        }

        const VkImageView attachments[] = {_color.view, _depth.view};
        VkFramebufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = _renderPass;
        info.attachmentCount = 2;
        info.pAttachments = attachments;
        info.width = width;
        info.height = height;
        info.layers = 1;
        if (vkCreateFramebuffer(_device, &info, nullptr, &_framebuffer) != VK_SUCCESS)
        {
            _framebuffer = VK_NULL_HANDLE;
            return false;
        }

        if (!CreateReadbackBuffer(static_cast<VkDeviceSize>(width) * height * 4))
        {
            return false;
        }
        return ClearTargetToBlack();
    }();

    if (!ok)
    {
        DestroyTargets();
        if (_initStatus == InitStatus::NotInitialized || _initStatus == InitStatus::Ok)
        {
            Fail(InitStatus::Failed, "could not create the " + std::to_string(width) + "x" + std::to_string(height) +
                                         " render target (out of memory?)");
        }
        return false;
    }
    _targetWidth = width;
    _targetHeight = height;
    return true;
}

void VulkanRenderer::DestroyTargets()
{
    if (_device == VK_NULL_HANDLE)
    {
        return;
    }
    if (_framebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(_device, _framebuffer, nullptr);
        _framebuffer = VK_NULL_HANDLE;
    }
    DestroyReadbackBuffer();
    DestroyImage(_depth);
    DestroyImage(_color);
    _targetWidth = 0;
    _targetHeight = 0;
}

void VulkanRenderer::AbandonOpenFrame()
{
    if (!_frameOpen)
    {
        return;
    }
    // Recorded against the old targets and never submitted: drop it
    vkResetCommandBuffer(_frames[_currentFrame].commandBuffer, 0);
    _frameOpen = false;
}

bool VulkanRenderer::RecreateTargets(uint32_t width, uint32_t height)
{
    AbandonOpenFrame();
    vkDeviceWaitIdle(_device); // nothing may still use the old images, framebuffer or readback buffer
    DestroyTargets();
    const InitStatus statusBefore = _initStatus;
    const std::string messageBefore = _initMessage;
    const bool ok = CreateTargets(width, height);
    // A failed resize is not an initialisation failure: it reports through the return value and the log only
    _initStatus = statusBefore;
    _initMessage = messageBefore;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::Shutdown()
{
    std::lock_guard lock(_mutex);
    ShutdownLocked();
    _initStatus = InitStatus::NotInitialized;
    _initMessage.clear();
    EndLifetime();
}

void VulkanRenderer::ShutdownLocked()
{
    if (_device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(_device);
        DestroyTargets();
        if (_renderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(_device, _renderPass, nullptr);
        }
        for (FrameResources& frame : _frames)
        {
            if (frame.fence != VK_NULL_HANDLE)
            {
                vkDestroyFence(_device, frame.fence, nullptr);
            }
            frame = {}; // the command buffers are freed with their pool
        }
        if (_commandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(_device, _commandPool, nullptr);
        }
        vkDestroyDevice(_device, nullptr); // with validation on, the layer reports every object not destroyed by now
    }
    if (_instance != VK_NULL_HANDLE)
    {
        if (_debugMessenger != VK_NULL_HANDLE)
        {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr)
            {
                destroy(_instance, _debugMessenger, nullptr);
            }
        }
        vkDestroyInstance(_instance, nullptr);
    }

    _debugMessenger = VK_NULL_HANDLE;
    _instance = VK_NULL_HANDLE;
    _physicalDevice = VK_NULL_HANDLE;
    _deviceProperties = {};
    _device = VK_NULL_HANDLE;
    _graphicsQueue = VK_NULL_HANDLE;
    _graphicsFamily = 0;
    _depthFormat = VK_FORMAT_UNDEFINED;
    _commandPool = VK_NULL_HANDLE;
    _renderPass = VK_NULL_HANDLE;
    _currentFrame = 0;
    _frameOpen = false;
    _deviceLost = false;
    _targetWidth = 0;
    _targetHeight = 0;
    _targetFixed = false;
    _validationEnabled = false;
    _initialized = false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::Clear(float r, float g, float b, float a)
{
    std::lock_guard lock(_mutex);
    _clearColor[0] = r;
    _clearColor[1] = g;
    _clearColor[2] = b;
    _clearColor[3] = a;

    if (_frameOpen)
    {
        // Like glClear in the middle of a frame: clears what has been drawn so far, colour and depth
        std::array<VkClearAttachment, 2> clears{};
        clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clears[0].colorAttachment = 0;
        std::copy(std::begin(_clearColor), std::end(_clearColor), clears[0].clearValue.color.float32);
        clears[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clears[1].clearValue.depthStencil = {1.0f, 0};
        VkClearRect rect{};
        rect.rect.extent = {_targetWidth, _targetHeight};
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        vkCmdClearAttachments(_frames[_currentFrame].commandBuffer, static_cast<uint32_t>(clears.size()),
                              clears.data(), 1, &rect);
    }
}

void VulkanRenderer::BeginFrame()
{
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost || _framebuffer == VK_NULL_HANDLE)
    {
        return;
    }
    if (_frameOpen)
    {
        LogUnsupportedOnce("BeginFrame while a frame is already open (ignored)");
        return;
    }

    FrameResources& frame = _frames[_currentFrame];
    // At most MAX_FRAMES_IN_FLIGHT frames are pending: wait for the frame that used this slot
    const VkResult waited = vkWaitForFences(_device, 1, &frame.fence, VK_TRUE, FenceTimeoutNanoseconds);
    if (waited != VK_SUCCESS)
    {
        _deviceLost = true;
        std::cerr << "Vulkan renderer: waiting for a frame failed (result " << waited << "); frames are skipped"
                  << std::endl;
        return;
    }
    vkResetCommandBuffer(frame.commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS)
    {
        _deviceLost = true;
        std::cerr << "Vulkan renderer: vkBeginCommandBuffer failed; frames are skipped" << std::endl;
        return;
    }

    std::array<VkClearValue, 2> clearValues{};
    std::copy(std::begin(_clearColor), std::end(_clearColor), clearValues[0].color.float32);
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = _renderPass;
    passInfo.framebuffer = _framebuffer;
    passInfo.renderArea.offset = {0, 0};
    passInfo.renderArea.extent = {_targetWidth, _targetHeight};
    passInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    passInfo.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(frame.commandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    _frameOpen = true;
}

void VulkanRenderer::EndFrame()
{
    std::lock_guard lock(_mutex);
    if (!_frameOpen)
    {
        return;
    }

    FrameResources& frame = _frames[_currentFrame];
    _frameOpen = false;
    vkCmdEndRenderPass(frame.commandBuffer);
    if (vkEndCommandBuffer(frame.commandBuffer) != VK_SUCCESS)
    {
        _deviceLost = true;
        std::cerr << "Vulkan renderer: vkEndCommandBuffer failed; frames are skipped" << std::endl;
        return;
    }

    // Reset right before the submit that signals it again, so a frame that is never submitted never leaves the slot's
    // fence unsignalled (the next BeginFrame of the slot would wait on it forever)
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    if (vkResetFences(_device, 1, &frame.fence) != VK_SUCCESS ||
        vkQueueSubmit(_graphicsQueue, 1, &submitInfo, frame.fence) != VK_SUCCESS)
    {
        _deviceLost = true;
        std::cerr << "Vulkan renderer: vkQueueSubmit failed; frames are skipped" << std::endl;
        return;
    }
    _currentFrame = (_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderer::Present()
{
    // No swapchain yet (stage F of #43): a frame is only ever read back, so there is nothing to present. Say so once per
    // renderer (windowed or not), because with a window the user would otherwise just see it stay blank
    {
        std::lock_guard lock(_mutex);
        if (_warnedWindow)
        {
            return;
        }
        _warnedWindow = true;
    }
    std::cerr << "Vulkan renderer: Vulkan swapchain/present is not implemented yet (stage F): frames render offscreen "
                 "and the window stays blank"
              << std::endl;
}

void VulkanRenderer::ReadFramebuffer(std::uint8_t* buffer, int width, int height) const
{
    if (buffer == nullptr || width <= 0 || height <= 0)
    {
        return;
    }
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost || _color.image == VK_NULL_HANDLE || _readback.mapped == nullptr)
    {
        return;
    }

    const uint32_t sourceWidth = _targetWidth;
    const uint32_t sourceHeight = _targetHeight;
    const VkImage image = _color.image;
    const VkBuffer readbackBuffer = _readback.buffer;

    const bool copied = SubmitOneTime(
        [=](VkCommandBuffer commandBuffer)
        {
            // The last frame's writes (the render pass's out-dependency already covers it) are visible to the copy
            VkImageMemoryBarrier barrier = ColorBarrier(
                image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0; // tightly packed
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {sourceWidth, sourceHeight, 1};
            vkCmdCopyImageToBuffer(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1,
                                   &region);

            // The copy's writes are visible to the host once the queue is idle
            VkBufferMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = readbackBuffer;
            toHost.offset = 0;
            toHost.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                 nullptr, 1, &toHost, 0, nullptr);
        });
    if (!copied)
    {
        std::cerr << "Vulkan renderer: ReadFramebuffer could not copy the frame" << std::endl;
        return;
    }

    if (!_readback.coherent)
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = _readback.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(_device, 1, &range);
    }

    // The image's row 0 is the top of the picture; the interface wants the bottom row first. A different size is
    // resampled (nearest, from pixel centres), as the software renderer does.
    const auto* source = static_cast<const std::uint8_t*>(_readback.mapped);
    const std::size_t outputWidth = static_cast<std::size_t>(width);
    const std::size_t outputHeight = static_cast<std::size_t>(height);
    if (outputWidth == sourceWidth && outputHeight == sourceHeight)
    {
        const std::size_t rowBytes = static_cast<std::size_t>(sourceWidth) * 4;
        for (std::size_t y = 0; y < outputHeight; ++y)
        {
            std::memcpy(buffer + y * rowBytes, source + (outputHeight - 1 - y) * rowBytes, rowBytes);
        }
        return;
    }
    for (std::size_t y = 0; y < outputHeight; ++y)
    {
        const std::size_t sourceYFromBottom = (2 * y + 1) * sourceHeight / (2 * outputHeight);
        const std::size_t sourceRow = sourceHeight - 1 - sourceYFromBottom;
        for (std::size_t x = 0; x < outputWidth; ++x)
        {
            const std::size_t sourceX = (2 * x + 1) * sourceWidth / (2 * outputWidth);
            std::memcpy(buffer + (y * outputWidth + x) * 4, source + (sourceRow * sourceWidth + sourceX) * 4, 4);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::Resize(uint32_t width, uint32_t height)
{
    std::lock_guard lock(_mutex);
    if (width == 0 || height == 0)
    {
        return; // a minimised window
    }
    _requestedWidth = width;
    _requestedHeight = height;
    if (!_initialized || _deviceLost || _targetFixed || (width == _targetWidth && height == _targetHeight))
    {
        return;
    }
    if (!RecreateTargets(width, height))
    {
        std::cerr << "Vulkan renderer: could not resize the render target to " << width << "x" << height
                  << "; frames are skipped until it can" << std::endl;
    }
}

void VulkanRenderer::OnResize(int width, int height)
{
    if (width > 0 && height > 0)
    {
        Resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

bool VulkanRenderer::SetRenderTargetSize(uint32_t width, uint32_t height)
{
    std::lock_guard lock(_mutex);
    if (width == 0 || height == 0 || !_initialized || _deviceLost)
    {
        return false;
    }
    _targetFixed = true;
    if (_framebuffer != VK_NULL_HANDLE && width == _targetWidth && height == _targetHeight)
    {
        return true; // the same size again keeps the target
    }
    if (!RecreateTargets(width, height))
    {
        std::cerr << "Vulkan renderer: could not make a render target of " << width << "x" << height << std::endl;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Stubs: stages A-F of #43. Each fails loudly (a log, once) and cleanly (nullptr/false/nothing), never crashes.
// ---------------------------------------------------------------------------------------------------------------------

Renderer::Common::IMesh* VulkanRenderer::CreateMesh(const Common::MeshData& meshData)
{
    static_cast<void>(meshData);
    LogUnsupportedOnce("CreateMesh");
    return nullptr;
}

void VulkanRenderer::DestroyMesh(Common::IMesh* mesh)
{
    static_cast<void>(mesh); // this renderer made none
}

Renderer::Common::ITexture* VulkanRenderer::CreateTexture(const uint8_t* data, uint32_t width, uint32_t height,
                                                uint32_t channels)
{
    static_cast<void>(data);
    static_cast<void>(width);
    static_cast<void>(height);
    static_cast<void>(channels);
    LogUnsupportedOnce("CreateTexture");
    return nullptr;
}

Renderer::Common::ITexture* VulkanRenderer::CreateTexture(const uint8_t* data, uint32_t width, uint32_t height,
                                                uint32_t channels, const Common::TextureOptions& options)
{
    static_cast<void>(options);
    return CreateTexture(data, width, height, channels);
}

void VulkanRenderer::DestroyTexture(Common::ITexture* texture)
{
    static_cast<void>(texture);
}

bool VulkanRenderer::UpdateMesh(Common::IMesh* mesh, const Common::MeshData& meshData)
{
    static_cast<void>(mesh);
    static_cast<void>(meshData);
    LogUnsupportedOnce("UpdateMesh");
    return false;
}

Renderer::Common::IMaterial* VulkanRenderer::CreateMaterial(Common::IShader* shader)
{
    return CreateMaterial(shader, nullptr);
}

Renderer::Common::IMaterial* VulkanRenderer::CreateMaterial(Common::IShader* shader, Common::ITexture* texture)
{
    static_cast<void>(shader);
    static_cast<void>(texture);
    LogUnsupportedOnce("CreateMaterial");
    return nullptr;
}

void VulkanRenderer::DestroyMaterial(Common::IMaterial* material)
{
    static_cast<void>(material);
}

Renderer::Common::IShader* VulkanRenderer::CreateShaderProgram(const char* vertexSource, const char* fragmentSource)
{
    static_cast<void>(vertexSource);
    static_cast<void>(fragmentSource);
    LogUnsupportedOnce("CreateShaderProgram");
    return nullptr;
}

void VulkanRenderer::UseShaderProgram(Common::IShader* shader)
{
    static_cast<void>(shader);
}

bool VulkanRenderer::DestroyShaderProgram(Common::IShader* shader)
{
    static_cast<void>(shader);
    return false;
}

bool VulkanRenderer::IsValidShader(Common::IShader* shader) const
{
    static_cast<void>(shader);
    return false;
}

Renderer::Common::IShader* VulkanRenderer::GetStandardUnlitShader() const
{
    LogUnsupportedOnce("GetStandardUnlitShader");
    return nullptr;
}

Renderer::Common::IShader* VulkanRenderer::GetStandardLitShader() const
{
    LogUnsupportedOnce("GetStandardLitShader");
    return nullptr;
}

void VulkanRenderer::SetViewProjection(const float* view, const float* projection)
{
    static_cast<void>(view);
    static_cast<void>(projection);
}

void VulkanRenderer::UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                         const N2Engine::Math::Vector3& cameraPosition)
{
    static_cast<void>(lighting);
    static_cast<void>(cameraPosition);
}

void VulkanRenderer::DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                              const Common::RenderState& state)
{
    // Nothing is drawn yet (#43). When it is, `state` selects the pipeline's depth/cull/blend settings.
    static_cast<void>(mesh);
    static_cast<void>(modelMatrix);
    static_cast<void>(material);
    static_cast<void>(state);
    LogUnsupportedOnce("DrawMesh");
}

void VulkanRenderer::DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                              const Common::RenderState& state, const Common::IndexRange& range)
{
    // Nothing is drawn yet (#43). When it is, `range` becomes the firstIndex and indexCount of vkCmdDrawIndexed.
    static_cast<void>(mesh);
    static_cast<void>(modelMatrix);
    static_cast<void>(material);
    static_cast<void>(state);
    static_cast<void>(range);
    LogUnsupportedOnce("DrawMesh");
}

void VulkanRenderer::DrawObjects(const std::vector<Common::RenderObject>& objects)
{
    if (!objects.empty())
    {
        LogUnsupportedOnce("DrawObjects");
    }
}

void VulkanRenderer::SetWireframe(bool enabled)
{
    static_cast<void>(enabled);
}

std::vector<GPUInfo> VulkanRenderer::GetCompatibleGPUs()
{
    return {}; // stage F
}

bool VulkanRenderer::SelectGPU(VkPhysicalDevice device)
{
    static_cast<void>(device);
    return false; // stage F
}

VkPhysicalDevice VulkanRenderer::GetRecommendedGPU(const std::vector<GPUInfo>& gpus)
{
    static_cast<void>(gpus);
    return VK_NULL_HANDLE; // stage F
}
