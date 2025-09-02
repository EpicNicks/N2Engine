#include "renderer/vulkan/VulkanRenderer.hpp"

#include <cstring>
#include <cstdint>
#include <functional>
#include <span>
#include <iostream>

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

bool VulkanRenderer::CreateInstance()
{
    if (ENABLE_VALIDATION_LAYERS && !CheckValidationLayerSupport())
    {
        return false;
    }

    VkApplicationInfo vkAppInfo{};
    vkAppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    vkAppInfo.pApplicationName = "Vulkan Renderer";
    vkAppInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    vkAppInfo.pEngineName = "Custom Engine";
    vkAppInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    vkAppInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo vkCreateInfo{};
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

bool VulkanRenderer::SetupDebugMessenger()
{
    if (!ENABLE_VALIDATION_LAYERS)
        return true;

    VkDebugUtilsMessengerCreateInfoEXT vkCreateInfo{};
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