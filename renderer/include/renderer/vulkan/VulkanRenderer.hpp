#pragma once

#include "renderer/common/Renderer.hpp"

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <vector>
#include <optional>
#include <unordered_map>
#include <string>
#include <array>

namespace Renderer
{
    namespace Vulkan
    {
        struct QueueFamilyIndices
        {
            std::optional<uint32_t> graphicsFamily;
            std::optional<uint32_t> presentFamily;

            bool IsComplete() const
            {
                return graphicsFamily.has_value() && presentFamily.has_value();
            }
        };

        struct SwapChainSupportDetails
        {
            VkSurfaceCapabilitiesKHR capabilities;
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };

        struct UniformBufferObject
        {
            alignas(16) float model[16];
            alignas(16) float view[16];
            alignas(16) float proj[16];
        };

        struct VulkanBuffer
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            void *mapped = nullptr;
        };

        struct VulkanTexture
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            uint32_t width = 0;
            uint32_t height = 0;
        };

        struct VulkanMesh
        {
            VulkanBuffer vertexBuffer;
            VulkanBuffer indexBuffer;
            uint32_t indexCount = 0;
        };

        struct VulkanMaterial
        {
            uint32_t textureId;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        };

        struct GPUInfo
        {
            VkPhysicalDevice device;
            std::string name;
            VkPhysicalDeviceType type;
            uint64_t vramMB;
            uint32_t vendorId;
            bool isSupported;

            uint32_t driverVersion;
            uint32_t apiVersion;
            VkPhysicalDeviceLimits limits;
        };

        class VulkanRenderer : public Renderer::Common::IRenderer
        {
        public:
            VulkanRenderer();
            ~VulkanRenderer() override;

            // IRenderer implementation
            bool Initialize(GLFWwindow *windowHandle, uint32_t width, uint32_t height) override;
            void Shutdown() override;
            void Resize(uint32_t width, uint32_t height) override;
            void Clear(float r, float g, float b, float a) override;

            void BeginFrame() override;
            void EndFrame() override;
            void Present() override;

            uint32_t CreateMesh(const Renderer::Common::MeshData &meshData) override;
            void DestroyMesh(uint32_t meshId) override;
            uint32_t CreateTexture(const uint8_t *data, uint32_t width, uint32_t height, uint32_t channels) override;
            void DestroyTexture(uint32_t textureId) override;
            Common::IMaterial *CreateMaterial(uint32_t shaderId, uint32_t textureId = 0) override;
            void DestroyMaterial(Renderer::Common::IMaterial *material) override;

            uint32_t CreateShaderProgram(const char *vertexSource, const char *fragmentSource) override;
            void UseShaderProgram(uint32_t shaderId) override;
            void DestroyShaderProgram(uint32_t shaderId) override;
            bool IsValidShader(uint32_t shaderId) const override;

            void SetViewProjection(const float *view, const float *projection) override;
            void DrawMesh(uint32_t meshId, const float *modelMatrix, Common::IMaterial *material) override;
            void DrawObjects(const std::vector<Renderer::Common::RenderObject> &objects) override;
            void OnResize(int width, int height) override;

            void SetWireframe(bool enabled) override;
            const char *GetRendererName() const override { return "Vulkan Renderer"; }

            std::vector<GPUInfo> GetCompatibleGPUs();
            bool SelectGPU(VkPhysicalDevice device);
            VkPhysicalDevice GetRecommendedGPU(const std::vector<GPUInfo> &gpus);

        private:
            // Initialization
            bool CreateInstance();
            bool SetupDebugMessenger();
            bool CreateSurface(GLFWwindow *windowHandle);
            bool PickPhysicalDevice();
            uint32_t ScoreDevice(VkPhysicalDevice device);
            bool CreateLogicalDevice();
            bool CreateSwapChain();
            bool CreateImageViews();
            bool CreateRenderPass();
            bool CreateDescriptorSetLayout();
            bool CreateGraphicsPipeline();
            bool CreateCommandPool();
            bool CreateDepthResources();
            bool CreateFramebuffers();
            bool CreateUniformBuffers();
            bool CreateDescriptorPool();
            bool CreateCommandBuffers();
            bool CreateSyncObjects();

            // Helper functions
            QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
            SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device);
            VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &availableFormats);
            VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR> &availablePresentModes);
            VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities);
            bool IsDeviceSuitable(VkPhysicalDevice device);
            bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
            std::vector<const char *> GetRequiredExtensions();
            bool CheckValidationLayerSupport();

            // Resource management
            bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                              VkBuffer &buffer, VkDeviceMemory &bufferMemory);
            void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
            uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

            // Texture helpers
            bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
                             VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                             VkImage &image, VkDeviceMemory &imageMemory);
            VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
            void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
            void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
            VkFormat FindDepthFormat();
            VkFormat FindSupportedFormat(const std::vector<VkFormat> &candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
            bool HasStencilComponent(VkFormat format);

            // Command buffer helpers
            VkCommandBuffer BeginSingleTimeCommands();
            void EndSingleTimeCommands(VkCommandBuffer commandBuffer);

            // Cleanup
            void CleanupSwapChain();
            void RecreateSwapChain();

            // Debug callback
            static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
                VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                VkDebugUtilsMessageTypeFlagsEXT messageType,
                const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                void *pUserData);

        private:
            // Core Vulkan objects
            VkInstance _instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
            VkSurfaceKHR _surface = VK_NULL_HANDLE;
            VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
            VkDevice _device = VK_NULL_HANDLE;
            VkQueue _graphicsQueue = VK_NULL_HANDLE;
            VkQueue _presentQueue = VK_NULL_HANDLE;

            // Swap chain
            VkSwapchainKHR _swapChain = VK_NULL_HANDLE;
            std::vector<VkImage> _swapChainImages;
            VkFormat _swapChainImageFormat;
            VkExtent2D _swapChainExtent;
            std::vector<VkImageView> _swapChainImageViews;
            std::vector<VkFramebuffer> _swapChainFramebuffers;

            // Pipeline
            VkRenderPass _renderPass = VK_NULL_HANDLE;
            VkDescriptorSetLayout _descriptorSetLayout = VK_NULL_HANDLE;
            VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
            VkPipeline _graphicsPipeline = VK_NULL_HANDLE;

            // Command pools and buffers
            VkCommandPool _commandPool = VK_NULL_HANDLE;
            std::vector<VkCommandBuffer> _commandBuffers;

            // Synchronization
            std::vector<VkSemaphore> _imageAvailableSemaphores;
            std::vector<VkSemaphore> _renderFinishedSemaphores;
            std::vector<VkFence> _inFlightFences;

            // Uniform buffers
            std::vector<VulkanBuffer> _uniformBuffers;
            std::vector<VkDescriptorSet> _descriptorSets;
            VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;

            // Depth buffer
            VkImage _depthImage = VK_NULL_HANDLE;
            VkDeviceMemory _depthImageMemory = VK_NULL_HANDLE;
            VkImageView _depthImageView = VK_NULL_HANDLE;

            // Resource storage
            std::unordered_map<uint32_t, VulkanMesh> _meshes;
            std::unordered_map<uint32_t, VulkanTexture> _textures;
            std::unordered_map<uint32_t, VulkanMaterial> _materials;
            uint32_t _nextMeshId = 1;
            uint32_t _nextTextureId = 1;
            uint32_t _nextMaterialId = 1;

            // Frame state
            uint32_t _currentFrame = 0;
            uint32_t _imageIndex = 0;
            bool _framebufferResized = false;

            // View/projection matrices
            UniformBufferObject _ubo{};

            float _clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

            // Constants
            static const int MAX_FRAMES_IN_FLIGHT = 2;
            static const bool ENABLE_VALIDATION_LAYERS = true;

            const std::vector<const char *> _validationLayers = {
                "VK_LAYER_KHRONOS_validation"};

            const std::vector<const char *> _deviceExtensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        };

    }
}