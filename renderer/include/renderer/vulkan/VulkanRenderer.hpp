#pragma once

#include <vector>
#include <optional>
#include <unordered_map>
#include <string>
#include <array>
#include <memory>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "renderer/common/Renderer.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/IMesh.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/common/IMaterial.hpp"

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
            void* mapped = nullptr;
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

        // Forward declarations for Vulkan implementations
        class VulkanMesh;
        class VulkanTexture;
        class VulkanMaterial;
        class VulkanShader;

        class VulkanRenderer : public Renderer::Common::IRenderer
        {
        public:
            VulkanRenderer();
            ~VulkanRenderer() override;

            // IRenderer implementation
            bool Initialize(GLFWwindow* windowHandle, uint32_t width, uint32_t height) override;
            void Shutdown() override;
            void Resize(uint32_t width, uint32_t height) override;
            void Clear(float r, float g, float b, float a) override;

            void BeginFrame() override;
            void EndFrame() override;
            void Present() override;

            // Resource management - updated to use interface pointers
            Common::IMesh* CreateMesh(const Common::MeshData& meshData) override;
            void DestroyMesh(Common::IMesh* mesh) override;

            Common::ITexture*
            CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels) override;
            void DestroyTexture(Common::ITexture* texture) override;

            Common::IMaterial* CreateMaterial(Common::IShader* shader, Common::ITexture* texture = nullptr) override;
            void DestroyMaterial(Common::IMaterial* material) override;

            Common::IShader* CreateShaderProgram(const char* vertexSource, const char* fragmentSource) override;
            void UseShaderProgram(Common::IShader* shader) override;
            bool DestroyShaderProgram(Common::IShader* shader) override;
            bool IsValidShader(Common::IShader* shader) const override;

            void SetViewProjection(const float* view, const float* projection) override;
            void UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                     const N2Engine::Math::Vector3& cameraPosition) override;

            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material) override;
            void DrawObjects(const std::vector<Common::RenderObject>& objects) override;
            void OnResize(int width, int height) override;

            void SetWireframe(bool enabled) override;
            [[nodiscard]] const char* GetRendererName() const override { return "Vulkan Renderer"; }

            std::vector<GPUInfo> GetCompatibleGPUs();
            bool SelectGPU(VkPhysicalDevice device);
            VkPhysicalDevice GetRecommendedGPU(const std::vector<GPUInfo>& gpus);

            [[nodiscard]] Common::IShader* GetStandardUnlitShader() const override;
            [[nodiscard]] Common::IShader* GetStandardLitShader() const override;

        private:
            // Initialization
            bool CreateInstance();
            bool SetupDebugMessenger();
            bool CreateSurface(GLFWwindow* windowHandle);
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
            VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
            VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
            VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
            bool IsDeviceSuitable(VkPhysicalDevice device);
            bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
            std::vector<const char*> GetRequiredExtensions();
            bool CheckValidationLayerSupport();

            // Resource management
            bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                              VkBuffer& buffer, VkDeviceMemory& bufferMemory);
            void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
            uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

            // Texture helpers
            bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
                             VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                             VkImage& image, VkDeviceMemory& imageMemory);
            VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
            void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout,
                                       VkImageLayout newLayout);
            void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
            VkFormat FindDepthFormat();
            VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling,
                                         VkFormatFeatureFlags features);
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
                const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                void* pUserData);

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

            // Resource storage doesn't exist yet
            // std::unordered_map<Common::IShader *, std::shared_ptr<VulkanShader>> _shaderPrograms;
            // std::unordered_map<Common::IMesh *, std::unique_ptr<VulkanMesh>> _meshes;
            // std::unordered_map<Common::ITexture *, std::unique_ptr<VulkanTexture>> _textures;
            // std::unordered_map<Common::IMaterial *, std::unique_ptr<VulkanMaterial>> _materials;

            // Standard shaders
            Common::IShader* _standardUnlitShader = nullptr;
            Common::IShader* _standardLitShader = nullptr;

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

            const std::vector<const char*> _validationLayers = {
                "VK_LAYER_KHRONOS_validation"
            };

            const std::vector<const char*> _deviceExtensions = {
                VK_KHR_SWAPCHAIN_EXTENSION_NAME
            };
        };
    }
}
