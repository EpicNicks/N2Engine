#pragma once

#include <array>
#include <cstdint>
#include <functional>
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

        /// What a test can ask about a texture (GetTextureInfoForTest); `valid` is false for a pointer this renderer
        /// did not make (or already destroyed)
        struct TextureInfoForTest
        {
            bool valid = false;
            VkFormat format = VK_FORMAT_UNDEFINED; // the image's format (3 channels are stored as RGBA8)
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t channels = 0; // as given to CreateTexture
            uint32_t mipLevels = 0;
            VkFilter filter = VK_FILTER_NEAREST; // min and mag filter of the sampler
            VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT; // U, V and W
            VkSampler sampler = VK_NULL_HANDLE; // the cached sampler (shared between textures with equal options)
            VkComponentMapping swizzle{};       // the image view's: 1 channel (r,0,0,1), 2 channels (r,g,0,1)
        };

        struct DebugState;    // the messenger's counters, shared with the callback (defined in the .cpp)
        struct ResourceState; // meshes, textures, samplers, the VMA allocator and the deletion queues (private header)

        /**
         * The Vulkan backend, stages 0 and A of #43: initialisation, an offscreen colour and depth target, a frame that
         * clears it, readback, resizing and shutdown (stage 0), and meshes and textures (stage A: GPU memory from
         * VulkanMemoryAllocator, staging uploads, samplers, blit mipmaps, deferred deletion). It renders ONLY
         * offscreen (R8G8B8A8_UNORM colour, a depth target), with or without a window: there is no surface or
         * swapchain yet, so Present puts nothing on a window (stage F). Initialize(nullptr, w, h) works with no
         * window and no display.
         *
         * Meshes and textures work but nothing draws them yet: materials, shaders, draws and lighting are stubs that
         * log once and return nullptr/false (stages B-E). All public methods may be called from any thread (one
         * mutex).
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
            /// Nothing to present until stage F (the frame is only readable through ReadFramebuffer): logs a warning to
            /// stderr the first time, once per renderer, whether or not there is a window
            void Present() override;

            // Meshes and textures (stage A). Uploads are synchronous (a queue stall per call) and work inside or
            // outside a frame. Destroying, or replacing through UpdateMesh, never frees GPU memory a frame in flight
            // may read: it is freed when that frame's slot comes round again (deferred deletion).
            /// A device-local vertex buffer (64-byte Vertex) and uint32 index buffer. nullptr for no vertices, or when
            /// the renderer is not initialized or the allocation fails. A mesh with vertices and no indices has an
            /// index count of 0 and no index buffer
            Common::IMesh* CreateMesh(const Common::MeshData& meshData) override;
            /// Ignores nullptr and any pointer this renderer did not make (another renderer's, another backend's, a
            /// fake): only meshes in this renderer's registry are destroyed
            void DestroyMesh(Common::IMesh* mesh) override;

            /// CreateTexture with TextureOptions::Default()
            Common::ITexture*
            CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels) override;
            /// 1 channel: R8 (samples (r,0,0,1)); 2: R8G8 ((r,g,0,1)); 3: expanded to RGBA8 with alpha 255; 4: RGBA8.
            /// srgb picks R8G8B8A8_SRGB for 3 and 4 channels (as OpenGL does; 1 and 2 channels ignore it) and falls
            /// back to UNORM when the device cannot sample it. Rows are tightly packed. A full mip chain (vkCmdBlitImage)
            /// only when the format supports linear blits, else one level. nullptr for null data, a zero size,
            /// channels outside 1-4, a size over the device's limit, or an uninitialized renderer
            Common::ITexture* CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels,
                                            const Common::TextureOptions& options) override;
            /// Ignores nullptr and pointers this renderer did not make, like DestroyMesh
            void DestroyTexture(Common::ITexture* texture) override;
            /// Keeps the same IMesh* and gives it NEW buffers (the old ones are retired, never written). False, with
            /// the mesh unchanged, for nullptr, a pointer this renderer did not make, or data with no vertices
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
            /// Zeroes the counters and forgets the stored messages (tests that provoke a message on purpose)
            void ResetValidationStats();
            /// Test hook: sends a synthetic message through the debug messenger (vkSubmitDebugUtilsMessageEXT), the
            /// path the validation layer reports through, so a test can prove the counters really count. False when
            /// there is no messenger (not initialized, or validation off)
            bool SubmitDebugMessageForTest(bool asError, const std::string& text);
            [[nodiscard]] std::string GetDeviceName() const;
            [[nodiscard]] VkPhysicalDeviceType GetDeviceType() const;
            /// The size of the offscreen target; 0 x 0 when there is none
            [[nodiscard]] uint32_t GetRenderWidth() const;
            [[nodiscard]] uint32_t GetRenderHeight() const;

            // Test hooks for stage A: nothing draws until stage B, so these are how a test proves what was uploaded
            /// Format, size, mip count and sampler of a texture this renderer made
            [[nodiscard]] TextureInfoForTest GetTextureInfoForTest(const Common::ITexture* texture) const;
            /// Copies mip level `mipLevel` of a texture into `bytes`, tightly packed in the image's format (RGBA8 for 3
            /// and 4 channels, R8, R8G8). False for a pointer that is not this renderer's or a level that does not exist
            bool ReadTextureForTest(const Common::ITexture* texture, uint32_t mipLevel, std::vector<uint8_t>& bytes,
                                    uint32_t* levelWidth = nullptr, uint32_t* levelHeight = nullptr);
            /// Copies a mesh's vertex and index buffers back. False for a pointer that is not this renderer's
            bool ReadMeshForTest(const Common::IMesh* mesh, Common::MeshData& out);
            /// Live VMA allocations: 2 per mesh with indices (1 without), 1 per texture, plus anything retired and
            /// not yet freed. Returns to its baseline once every frame slot has been flushed
            [[nodiscard]] uint32_t GetLiveAllocationCountForTest() const;
            [[nodiscard]] uint32_t GetLiveMeshCountForTest() const;
            [[nodiscard]] uint32_t GetLiveTextureCountForTest() const;
            /// Buffers and images waiting for a frame slot's fence (deferred deletion)
            [[nodiscard]] uint32_t GetPendingDeletionCountForTest() const;

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
            bool CreateAllocator();
            void DestroyAllocator();
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
            /// SubmitOneTime for the resource code in another translation unit
            bool SubmitOneTimeFn(const std::function<void(VkCommandBuffer)>& record) const;

            /// The frame slot whose fence guards what is retired now (see VulkanResources.cpp)
            [[nodiscard]] uint32_t RetireSlot() const;
            /// True when no frame is open and every frame fence is signalled, so the GPU cannot be using anything retired
            /// now and it can be freed at once instead of queued (see VulkanResources.cpp)
            [[nodiscard]] bool CanFreeRetiredNow() const;
            /// Frees what was retired into `slot`; called once that slot's fence has been waited for
            void FlushRetired(uint32_t slot);
            /// Device idle: frees every retired and every still-live mesh and texture and the samplers
            void DestroyAllResources();
            bool ClearTargetToBlack();
            void AbandonOpenFrame();
            void LogUnsupportedOnce(const char* what) const;
            /// Logs `message` to stderr the first time, once per renderer
            void LogOnce(const std::string& message) const;

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

            std::unique_ptr<ResourceState> _resources; // never null; its allocator is null until Initialize made one

            InitStatus _initStatus = InitStatus::NotInitialized;
            std::string _initMessage;
            bool _initialized = false;
            bool _warnedWindow = false;

            float _clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        };
    }
}
