#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Renderer.hpp first: it includes glad, which must come before GLFW's header (that one pulls in GL itself otherwise)
#include "renderer/common/Renderer.hpp"

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "renderer/common/IShader.hpp"
#include "renderer/common/IMesh.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/common/IMaterial.hpp"

namespace Renderer
{
    namespace Vulkan
    {
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

        /// A buffer and its memory; `mapped` is non-null while the memory stays mapped (the readback buffer)
        struct VulkanBuffer
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkDeviceSize size = 0;
            void* mapped = nullptr;
            bool coherent = true; // false: invalidate the mapped range before the host reads it
        };

        /// An image with its memory and a view of it
        struct VulkanImage
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
        };

        /// Whether the Khronos validation layer is loaded. Default: off in Release builds, on in Debug builds when
        /// the layer is installed (a missing layer then only logs a warning); the environment variable
        /// N2_VULKAN_VALIDATION=1 turns it on and =0 off for a renderer left at Default. Enabled demands the layer:
        /// Initialize fails with InitStatus::ValidationUnavailable when it is missing.
        enum class ValidationMode
        {
            Default,
            Enabled,
            Disabled,
        };

        /// What the debug messenger has counted since the renderer object was created (not reset by Shutdown or
        /// Initialize, so one number covers a whole test)
        struct ValidationStats
        {
            uint32_t errors = 0;        // every error, of any type (a leaked object at destroy is one)
            uint32_t warnings = 0;      // warnings of the validation and performance types
            uint32_t otherWarnings = 0; // general warnings (loader and driver chatter), logged only
        };

        /// Why the last Initialize ended as it did. The first three are about the machine, not the code: a test
        /// without a Vulkan device skips on them.
        enum class InitStatus
        {
            NotInitialized,
            Ok,
            NoVulkanRuntime,       // the loader found no usable driver (vkCreateInstance: incompatible driver)
            NoSuitableDevice,      // no physical device has a graphics queue and the formats the targets need
            ValidationUnavailable, // validation was demanded but the layer (or VK_EXT_debug_utils) is missing
            Failed,                // anything else: a bug or a resource failure
        };

        struct DebugState; // the messenger's counters, shared with the callback (defined in the .cpp)

        /**
         * The Vulkan backend, stage 0 of #43: initialisation, an offscreen colour and depth target, a frame that
         * clears it, readback, resizing and shutdown. It renders ONLY offscreen (R8G8B8A8_UNORM colour, a depth
         * target), with or without a window: there is no surface or swapchain yet, so Present puts nothing on a
         * window (stage F). Initialize(nullptr, w, h) works with no window and no display.
         *
         * Everything that would draw (meshes, textures, materials, shaders, draws, lighting) is a stub that logs
         * once and returns nullptr/false. All public methods may be called from any thread (one mutex).
         */
        class VulkanRenderer : public Renderer::Common::IRenderer
        {
        public:
            VulkanRenderer();
            ~VulkanRenderer() override;

            VulkanRenderer(const VulkanRenderer&) = delete;
            VulkanRenderer& operator=(const VulkanRenderer&) = delete;

            /// Before Initialize. See ValidationMode
            void SetValidationMode(ValidationMode mode);

            /// Creates the instance, device and an offscreen target of width x height (both non-zero; the window
            /// handle is accepted and not used yet). Returns false and leaves nothing allocated on any failure; see
            /// GetInitStatus. Initializing a renderer that is already initialized shuts it down first.
            bool Initialize(GLFWwindow* windowHandle, uint32_t width, uint32_t height) override;
            /// Destroys every Vulkan object and expires the lifetime token. Safe to call twice, or without Initialize
            void Shutdown() override;
            /// Sets the size of the offscreen target, unless SetRenderTargetSize fixed it (then ignored)
            void Resize(uint32_t width, uint32_t height) override;
            /// The colour the next BeginFrame clears to; between BeginFrame and EndFrame it also clears the frame so far
            void Clear(float r, float g, float b, float a) override;

            void BeginFrame() override;
            void EndFrame() override;
            /// Headless: nothing to present (the frame is only readable through ReadFramebuffer) until stage F
            void Present() override;

            // Resources: stubs until stages A-E (#43); each logs once and returns nullptr/false
            Common::IMesh* CreateMesh(const Common::MeshData& meshData) override;
            void DestroyMesh(Common::IMesh* mesh) override;

            Common::ITexture*
            CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels) override;
            Common::ITexture* CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels,
                                            const Common::TextureOptions& options) override;
            void DestroyTexture(Common::ITexture* texture) override;
            bool UpdateMesh(Common::IMesh* mesh, const Common::MeshData& meshData) override;

            Common::IMaterial* CreateMaterial(Common::IShader* shader) override;
            Common::IMaterial* CreateMaterial(Common::IShader* shader, Common::ITexture* texture) override;
            void DestroyMaterial(Common::IMaterial* material) override;

            Common::IShader* CreateShaderProgram(const char* vertexSource, const char* fragmentSource) override;
            void UseShaderProgram(Common::IShader* shader) override;
            bool DestroyShaderProgram(Common::IShader* shader) override;
            bool IsValidShader(Common::IShader* shader) const override;

            void SetViewProjection(const float* view, const float* projection) override;
            void UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                     const N2Engine::Math::Vector3& cameraPosition) override;

            using Common::IRenderer::DrawMesh; // the default-state overload
            // Draws nothing yet (stages B-D): logs once
            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                          const Common::RenderState& state) override;
            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                          const Common::RenderState& state, const Common::IndexRange& range) override;
            void DrawObjects(const std::vector<Common::RenderObject>& objects) override;
            /// Like Resize, from a window's framebuffer-size callback
            void OnResize(int width, int height) override;

            void SetWireframe(bool enabled) override;
            [[nodiscard]] const char* GetRendererName() const override { return "Vulkan Renderer"; }

            // GPU selection: stage F
            std::vector<GPUInfo> GetCompatibleGPUs();
            bool SelectGPU(VkPhysicalDevice device);
            VkPhysicalDevice GetRecommendedGPU(const std::vector<GPUInfo>& gpus);

            [[nodiscard]] Common::IShader* GetStandardUnlitShader() const override;
            [[nodiscard]] Common::IShader* GetStandardLitShader() const override;
            /// RGBA8, bottom row first, of the last frame (all black before the first). A size other than the
            /// target's is resampled, nearest, as the software renderer does. Writes nothing when not initialized or
            /// the target is gone.
            void ReadFramebuffer(std::uint8_t* buffer, int width, int height) const override;
            /// Recreates the offscreen target at width x height and keeps it at that size (Resize and OnResize then
            /// do nothing). False for a zero dimension (nothing changes), one over the device's limit, an
            /// uninitialized renderer, or an allocation failure (then the old target is gone and frames are skipped
            /// until a later call succeeds). The same size again keeps the target.
            [[nodiscard]] bool SetRenderTargetSize(uint32_t width, uint32_t height) override;

            // State for tests and diagnostics
            [[nodiscard]] InitStatus GetInitStatus() const;
            /// The reason of the last failed Initialize, or empty
            [[nodiscard]] std::string GetInitMessage() const;
            [[nodiscard]] bool IsValidationEnabled() const;
            [[nodiscard]] ValidationStats GetValidationStats() const;
            /// The first messages the validation layer reported (errors and warnings), up to 32
            [[nodiscard]] std::vector<std::string> GetValidationMessages() const;
            [[nodiscard]] std::string GetDeviceName() const;
            [[nodiscard]] VkPhysicalDeviceType GetDeviceType() const;
            /// The size of the offscreen target; 0 x 0 when there is none
            [[nodiscard]] uint32_t GetRenderWidth() const;
            [[nodiscard]] uint32_t GetRenderHeight() const;

        private:
            static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;

            struct FrameResources
            {
                VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
                VkFence fence = VK_NULL_HANDLE; // signalled while the frame's submission is not pending
            };

            // Everything below with "Locked" in a comment runs with _mutex held
            bool Fail(InitStatus status, const std::string& message);
            void ShutdownLocked();

            bool CreateInstance();
            bool SetupDebugMessenger();
            bool PickPhysicalDevice();
            bool CreateLogicalDevice();
            bool CreateCommandPool();
            bool CreateFrameResources();
            bool CreateRenderPass();
            bool CreateTargets(uint32_t width, uint32_t height);
            void DestroyTargets();
            bool RecreateTargets(uint32_t width, uint32_t height);

            bool CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                             VkImageAspectFlags aspect, VulkanImage& out);
            void DestroyImage(VulkanImage& image);
            bool CreateReadbackBuffer(VkDeviceSize size);
            void DestroyReadbackBuffer();
            bool AllocateMemory(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags required,
                                VkDeviceMemory& memory, VkMemoryPropertyFlags* chosenFlags = nullptr) const;

            /// Records `record` into a one-time command buffer, submits it and waits for the queue to go idle
            template <typename Recorder>
            bool SubmitOneTime(Recorder&& record) const;
            bool ClearTargetToBlack();
            void AbandonOpenFrame();
            void LogUnsupportedOnce(const char* what) const;

            [[nodiscard]] bool WantsValidation(bool& required) const;

        private:
            mutable std::mutex _mutex;
            mutable std::mutex _logMutex;
            mutable std::set<std::string> _loggedUnsupported;

            // Instance, device
            ValidationMode _validationMode = ValidationMode::Default;
            bool _validationEnabled = false;
            std::shared_ptr<DebugState> _debug;
            VkInstance _instance = VK_NULL_HANDLE;
            VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
            VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
            VkPhysicalDeviceProperties _deviceProperties{};
            VkDevice _device = VK_NULL_HANDLE;
            uint32_t _graphicsFamily = 0;
            VkQueue _graphicsQueue = VK_NULL_HANDLE;
            VkFormat _depthFormat = VK_FORMAT_UNDEFINED;
            static constexpr VkFormat ColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

            // Commands and frames in flight
            VkCommandPool _commandPool = VK_NULL_HANDLE;
            std::array<FrameResources, MAX_FRAMES_IN_FLIGHT> _frames{};
            uint32_t _currentFrame = 0;
            bool _frameOpen = false; // BeginFrame recorded the frame's commands and EndFrame has not submitted them
            bool _deviceLost = false;

            // The offscreen target
            VkRenderPass _renderPass = VK_NULL_HANDLE;
            VulkanImage _color;
            VulkanImage _depth;
            VkFramebuffer _framebuffer = VK_NULL_HANDLE;
            VulkanBuffer _readback;
            uint32_t _targetWidth = 0;
            uint32_t _targetHeight = 0;
            bool _targetFixed = false; // SetRenderTargetSize set the size: Resize and OnResize are ignored
            uint32_t _requestedWidth = 0; // the last size asked for through Initialize, Resize or OnResize
            uint32_t _requestedHeight = 0;

            InitStatus _initStatus = InitStatus::NotInitialized;
            std::string _initMessage;
            bool _initialized = false;
            bool _warnedWindow = false;

            float _clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        };
    }
}
