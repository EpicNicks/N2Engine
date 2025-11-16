#include <cstring>
#include <cstdint>
#include <functional>
#include <span>
#include <iostream>
#include <set>

#include "renderer/vulkan/VulkanRenderer.hpp"

using namespace Renderer::Vulkan;

VulkanRenderer::VulkanRenderer()
{
    std::memset(&_ubo, 0, sizeof(_ubo));

    for (std::size_t i = 0; i < 4; i++)
    {
        _ubo.model[i * 4 + i] = 1.0f;
        _ubo.view[i * 4 + i] = 1.0f;
        _ubo.proj[i * 4 + i] = 1.0f;
    }
}

VulkanRenderer::~VulkanRenderer()
{
    Shutdown();
}

void VulkanRenderer::Clear(float r, float g, float b, float a)
{
    _clearColor[0] = r;
    _clearColor[1] = g;
    _clearColor[2] = b;
    _clearColor[3] = a;
}

void Renderer::Vulkan::VulkanRenderer::BeginFrame()
{
}

void Renderer::Vulkan::VulkanRenderer::EndFrame()
{
}

void Renderer::Vulkan::VulkanRenderer::Present()
{
}

uint32_t Renderer::Vulkan::VulkanRenderer::CreateMesh(const Renderer::Common::MeshData &meshData)
{
    return 0;
}

void Renderer::Vulkan::VulkanRenderer::DestroyMesh(uint32_t meshId)
{
}

uint32_t Renderer::Vulkan::VulkanRenderer::CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels)
{
    return 0;
}

void Renderer::Vulkan::VulkanRenderer::DestroyTexture(uint32_t textureId)
{
}

Renderer::Common::IMaterial *Renderer::Vulkan::VulkanRenderer::CreateMaterial(Renderer::Common::IShader *shader, uint32_t textureId)
{
    return 0;
}

void Renderer::Vulkan::VulkanRenderer::DestroyMaterial(Renderer::Common::IMaterial *material)
{
}

void Renderer::Vulkan::VulkanRenderer::SetViewProjection(const float *view, const float *projection)
{
}

void Renderer::Vulkan::VulkanRenderer::DrawMesh(uint32_t meshId, const float *modelMatrix, Renderer::Common::IMaterial *material)
{
}

void Renderer::Vulkan::VulkanRenderer::DrawObjects(const std::vector<Renderer::Common::RenderObject> &objects)
{
}

void Renderer::Vulkan::VulkanRenderer::OnResize(int width, int height)
{
}
void Renderer::Vulkan::VulkanRenderer::SetWireframe(bool enabled)
{
}

std::vector<GPUInfo> Renderer::Vulkan::VulkanRenderer::GetCompatibleGPUs()
{
    return std::vector<GPUInfo>();
}

bool Renderer::Vulkan::VulkanRenderer::SelectGPU(VkPhysicalDevice device)
{
    return false;
}

VkPhysicalDevice Renderer::Vulkan::VulkanRenderer::GetRecommendedGPU(const std::vector<GPUInfo> &gpus)
{
    return VkPhysicalDevice();
}

bool VulkanRenderer::Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height)
{
    _swapChainExtent = {width, height};

    if (!CreateInstance())
        return false;
    if (!SetupDebugMessenger())
        return false;
    if (!CreateSurface(windowHandle))
        return false;
    if (!PickPhysicalDevice())
        return false;
    if (!CreateLogicalDevice())
        return false;
    if (!CreateSwapChain())
        return false;
    if (!CreateImageViews())
        return false;
    if (!CreateRenderPass())
        return false;
    if (!CreateDescriptorSetLayout())
        return false;
    if (!CreateGraphicsPipeline())
        return false;
    if (!CreateCommandPool())
        return false;
    if (!CreateDepthResources())
        return false;
    if (!CreateFramebuffers())
        return false;
    if (!CreateUniformBuffers())
        return false;
    if (!CreateDescriptorPool())
        return false;
    if (!CreateCommandBuffers())
        return false;
    if (!CreateSyncObjects())
        return false;

    return true;
}

void Renderer::Vulkan::VulkanRenderer::Shutdown()
{
}

void Renderer::Vulkan::VulkanRenderer::Resize(uint32_t width, uint32_t height)
{
}
bool VulkanRenderer::CreateInstance()
{
    if (ENABLE_VALIDATION_LAYERS && !CheckValidationLayerSupport())
    {
        return false;
    }

    VkApplicationInfo vkAppInfo;
    vkAppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    vkAppInfo.pApplicationName = "Vulkan Renderer";
    vkAppInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    vkAppInfo.pEngineName = "Custom Engine";
    vkAppInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    vkAppInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo vkCreateInfo;
    vkCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vkCreateInfo.pApplicationInfo = &vkAppInfo;

    auto extensions = GetRequiredExtensions();
    vkCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    vkCreateInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT vkDebugCreateInfo{};
    if (ENABLE_VALIDATION_LAYERS)
    {
        vkCreateInfo.enabledLayerCount = static_cast<uint32_t>(_validationLayers.size());
        vkCreateInfo.ppEnabledLayerNames = _validationLayers.data();

        vkDebugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        vkDebugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        vkDebugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        vkDebugCreateInfo.pfnUserCallback = DebugCallback;

        vkCreateInfo.pNext = &vkDebugCreateInfo;
    }
    return vkCreateInstance(&vkCreateInfo, nullptr, &_instance) == VK_SUCCESS;
}

std::vector<const char *> VulkanRenderer::GetRequiredExtensions()
{
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    std::vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

    if (ENABLE_VALIDATION_LAYERS)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    return extensions;
}

bool Renderer::Vulkan::VulkanRenderer::CheckValidationLayerSupport()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer &buffer, VkDeviceMemory &bufferMemory)
{
    return false;
}

void Renderer::Vulkan::VulkanRenderer::CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
}

uint32_t Renderer::Vulkan::VulkanRenderer::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
    return 0;
}

bool VulkanRenderer::SetupDebugMessenger()
{
    if (!ENABLE_VALIDATION_LAYERS)
        return true;

    VkDebugUtilsMessengerCreateInfoEXT vkCreateInfo;
    vkCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    vkCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    vkCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    vkCreateInfo.pfnUserCallback = DebugCallback;

    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr)
    {
        return func(_instance, &vkCreateInfo, nullptr, &_debugMessenger) == VK_SUCCESS;
    }

    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRenderer::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData)
{
    std::cerr << "Vulkan validation: " << pCallbackData->pMessage << std::endl;

    return VK_FALSE;
}

bool VulkanRenderer::CreateSurface(GLFWwindow *window)
{
    return glfwCreateWindowSurface(_instance, window, nullptr, &_surface) == VK_SUCCESS;
}

bool VulkanRenderer::PickPhysicalDevice()
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);

    if (deviceCount == 0)
    {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(_instance, &deviceCount, devices.data());

    VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
    uint32_t highestScore = 0;

    for (const auto &device : devices)
    {
        if (!IsDeviceSuitable(device))
        {
            continue;
        }

        uint32_t score = ScoreDevice(device);
        if (score > highestScore)
        {
            highestScore = score;
            bestDevice = device;
        }
    }

    if (bestDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    _physicalDevice = bestDevice;
    return true;
}

bool VulkanRenderer::IsDeviceSuitable(VkPhysicalDevice device)
{
    QueueFamilyIndices indices = FindQueueFamilies(device);
    bool extensionsSupported = CheckDeviceExtensionSupport(device);
    bool swapChainAdequate = false;
    if (extensionsSupported)
    {
        SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
        swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }

    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

    return indices.IsComplete() && extensionsSupported && swapChainAdequate && supportedFeatures.samplerAnisotropy;
}

bool Renderer::Vulkan::VulkanRenderer::CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
    return false;
}

uint32_t VulkanRenderer::ScoreDevice(VkPhysicalDevice device)
{
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    VkPhysicalDeviceMemoryProperties memProps;

    vkGetPhysicalDeviceProperties(device, &props);
    vkGetPhysicalDeviceFeatures(device, &features);
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);

    uint32_t score = 0;

    // Heavily favor discrete GPUs
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
        score += 10000;
    }
    else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
    {
        score += 1000; // Still usable but much lower priority
    }

    // Add VRAM amount to score (in MB)
    uint64_t totalVRAM = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
    {
        if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            totalVRAM += memProps.memoryHeaps[i].size / (1024 * 1024);
        }
    }
    score += static_cast<uint32_t>(totalVRAM);

    // Texture dimension support (higher = better)
    score += props.limits.maxImageDimension2D / 1000;

    // Geometry shader support bonus
    if (features.geometryShader)
    {
        score += 100;
    }

    return score;
}

// can return a partial struct of QueueFamily which will have IsComplete() is false
QueueFamilyIndices VulkanRenderer::FindQueueFamilies(VkPhysicalDevice device)
{
    QueueFamilyIndices indices;
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies{queueFamilyCount};
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

    int i = 0;
    for (const auto &queueFamily : queueFamilies)
    {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = false;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, _surface, &presentSupport);
        if (presentSupport)
        {
            indices.presentFamily = i;
        }
        if (indices.IsComplete())
        {
            break;
        }
        i++;
    }
    return indices;
}

SwapChainSupportDetails Renderer::Vulkan::VulkanRenderer::QuerySwapChainSupport(VkPhysicalDevice device)
{
    return SwapChainSupportDetails();
}

bool VulkanRenderer::CreateLogicalDevice()
{
    QueueFamilyIndices indices = FindQueueFamilies(_physicalDevice);
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {
        indices.graphicsFamily.value(),
        indices.presentFamily.value()};

    float queuePriority = 1.0f;

    for (uint32_t queueFamily : uniqueQueueFamilies)
    {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures;
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo createInfo;
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &deviceFeatures;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(_deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = _deviceExtensions.data();

    if (ENABLE_VALIDATION_LAYERS)
    {
        createInfo.enabledLayerCount = static_cast<uint32_t>(_validationLayers.size());
        createInfo.ppEnabledLayerNames = _validationLayers.data();
    }
    if (vkCreateDevice(_physicalDevice, &createInfo, nullptr, &_device) != VK_SUCCESS)
    {
        return false;
    }
    vkGetDeviceQueue(_device, indices.graphicsFamily.value(), 0, &_graphicsQueue);
    vkGetDeviceQueue(_device, indices.presentFamily.value(), 0, &_presentQueue);

    return true;
}

bool VulkanRenderer::CreateSwapChain()
{
    SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(_physicalDevice);

    VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
    VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.presentModes);
    VkExtent2D extent = ChooseSwapExtent(swapChainSupport.capabilities);

    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount)
    {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo;
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = _surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1; // 1 unless stereoscopic 3D
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices = FindQueueFamilies(_physicalDevice);
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily)
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    }
    else
    {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(_device, &createInfo, nullptr, &_swapChain) != VK_SUCCESS)
    {
        return false;
    }

    vkGetSwapchainImagesKHR(_device, _swapChain, &imageCount, nullptr);
    _swapChainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(_device, _swapChain, &imageCount, _swapChainImages.data());

    _swapChainImageFormat = surfaceFormat.format;
    _swapChainExtent = extent;

    return true;
}

VkSurfaceFormatKHR VulkanRenderer::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats)
{
    for (const auto &availableFormat : availableFormats)
    {
        if (availableFormat.format != VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

VkPresentModeKHR VulkanRenderer::ChooseSwapPresentMode(
    const std::vector<VkPresentModeKHR> &availablePresentModes)
{
    // Prefer triple buffering (mailbox mode) if available
    for (const auto &availablePresentMode : availablePresentModes)
    {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            return availablePresentMode;
        }
    }

    // Fallback to FIFO (vsync)
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Renderer::Vulkan::VulkanRenderer::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities)
{
    return VkExtent2D();
}
bool VulkanRenderer::CreateImageViews()
{
    _swapChainImageViews.resize(_swapChainImages.size());

    for (size_t i = 0; i < _swapChainImages.size(); i++)
    {
        _swapChainImageViews[i] = CreateImageView(_swapChainImages[i], _swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT);

        if (_swapChainImageViews[i] == VK_NULL_HANDLE)
        {
            return false;
        }
    }
    return true;
}

bool Renderer::Vulkan::VulkanRenderer::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage &image, VkDeviceMemory &imageMemory)
{
    return false;
}

VkImageView VulkanRenderer::CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags)
{
    VkImageViewCreateInfo viewInfo;
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView;
    if (vkCreateImageView(_device, &viewInfo, nullptr, &imageView) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return imageView;
}

void Renderer::Vulkan::VulkanRenderer::TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout)
{
}

void Renderer::Vulkan::VulkanRenderer::CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
}

VkFormat Renderer::Vulkan::VulkanRenderer::FindDepthFormat()
{
    return VkFormat();
}

VkFormat Renderer::Vulkan::VulkanRenderer::FindSupportedFormat(const std::vector<VkFormat> &candidates, VkImageTiling tiling, VkFormatFeatureFlags features)
{
    return VkFormat();
}

bool Renderer::Vulkan::VulkanRenderer::HasStencilComponent(VkFormat format)
{
    return false;
}

VkCommandBuffer Renderer::Vulkan::VulkanRenderer::BeginSingleTimeCommands()
{
    return VkCommandBuffer();
}

void Renderer::Vulkan::VulkanRenderer::EndSingleTimeCommands(VkCommandBuffer commandBuffer)
{
}

void Renderer::Vulkan::VulkanRenderer::CleanupSwapChain()
{
}

void Renderer::Vulkan::VulkanRenderer::RecreateSwapChain()
{
}

bool VulkanRenderer::CreateRenderPass()
{
    VkAttachmentDescription colorAttachment;
    colorAttachment.format = _swapChainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment;
    depthAttachment.format = FindDepthFormat();
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef;
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef;
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency;
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo renderPassInfo;
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    return vkCreateRenderPass(_device, &renderPassInfo, nullptr, &_renderPass) == VK_SUCCESS;
}

bool VulkanRenderer::CreateDescriptorSetLayout()
{
    // Uniform buffer binding (for MVP matrices)
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;
    // Texture sampler binding
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {uboLayoutBinding, samplerLayoutBinding};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    return vkCreateDescriptorSetLayout(_device, &layoutInfo, nullptr, &_descriptorSetLayout) == VK_SUCCESS;
}
// TODO: continue implementing, handle configurability

bool Renderer::Vulkan::VulkanRenderer::CreateGraphicsPipeline()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateCommandPool()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateDepthResources()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateFramebuffers()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateUniformBuffers()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateDescriptorPool()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateCommandBuffers()
{
    return false;
}

bool Renderer::Vulkan::VulkanRenderer::CreateSyncObjects()
{
    return false;
}

Renderer::Common::IShader *VulkanRenderer::CreateShaderProgram(const char *vertexSource, const char *fragmentSource)
{
    std::cerr << "VulkanRenderer::CreateShaderProgram not implemented yet" << std::endl;
    return 0; // Return 0 to indicate failure
}

void VulkanRenderer::UseShaderProgram(Renderer::Common::IShader *shaderId)
{
    std::cerr << "VulkanRenderer::UseShaderProgram not implemented yet" << std::endl;
}

bool VulkanRenderer::DestroyShaderProgram(Renderer::Common::IShader *shader)
{
    std::cerr << "VulkanRenderer::DestroyShaderProgram not implemented yet" << std::endl;
    return false;
}

bool VulkanRenderer::IsValidShader(uint32_t shaderId) const
{
    return false; // No shaders are valid in stub implementation
}

Renderer::Common::IShader *VulkanRenderer::GetStandardUnlitShader() const
{
    return nullptr; // Stub implementation
}

Renderer::Common::IShader *VulkanRenderer::GetStandardLitShader() const
{
    return nullptr; // Stub implementation
}