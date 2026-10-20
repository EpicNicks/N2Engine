#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Renderer.hpp first: it includes glad, which must come before GLFW's header (that one pulls in GL itself otherwise)
#include "renderer/common/Renderer.hpp"

// The C API only (webgpu.h, as Dawn ships it): no Dawn C++ header (webgpu_cpp.h, dawn/native/*) may be included from
// here or from renderer/src/webgpu/, because stage G4 compiles the same files with Emscripten's emdawnwebgpu port.
#include <webgpu/webgpu.h>

namespace Renderer
{
    namespace WebGpu
    {
        /// Which adapters Initialize may take. Default asks the environment: N2_WEBGPU_EXPECT_CPU_DEVICE=1 means CpuOnly,
        /// anything else means Any.
        enum class AdapterPreference
        {
            Default,
            Any,     // the Vulkan adapter Dawn prefers (a real GPU when there is one), else a CPU (fallback) adapter
            CpuOnly, // only a CPU adapter (Mesa lavapipe, SwiftShader): what CI pins, so a green run says which device ran
        };

        /// Why the last Initialize ended as it did. NoInstance and NoAdapter are about the machine, not the code: a test
        /// without a WebGPU adapter skips on them (and fails under N2_REQUIRE_WEBGPU=1).
        enum class InitStatus
        {
            NotInitialized,
            Ok,
            NoInstance, // wgpuCreateInstance returned null
            NoAdapter,  // Dawn found no adapter of the kind asked for (no Vulkan driver, or no CPU adapter under CpuOnly)
            Failed,     // anything else: no device, no queue, no target, or an error reported while creating them
        };

        /**
         * The WebGPU backend, stage G2 of #155: the core of a renderer that runs on Dawn (natively, for the CI harness only).
         * Headless initialisation (instance, adapter, device, queue), an offscreen RGBA8 colour target and a Depth32Float
         * depth target, a frame that clears them, a synchronous ReadFramebuffer, SetRenderTargetSize and Shutdown, plus
         * counting of every error the device reports. There is no window and no swapchain: Present only warns.
         *
         * NOT here yet: meshes, textures, materials, shaders and every draw (stages G3a and G3b). CreateMesh, CreateTexture,
         * CreateMaterial and CreateShaderProgram return nullptr, GetStandardUnlitShader, GetStandardLitShader and
         * GetStandardTextShader return nullptr, and DrawMesh, DrawObjects, UpdateMesh and SetWireframe do nothing; each logs
         * one warning per renderer. A frame still renders (it clears).
         *
         * Written against the C API (webgpu.h) alone, so the same file compiles with Emscripten's emdawnwebgpu in stage G4.
         * The pieces that exist only natively (the Vulkan backend request, the blocking waits) are marked in the source.
         * Only built with N2ENGINE_RENDERER_WEBGPU=ON (CI-only: never selectable from the editor host, lua_project or a
         * project's settings, design decision W8). All public methods may be called from any thread (one mutex).
         */
        class WebGpuRenderer : public Renderer::Common::IRenderer
        {
        public:
            WebGpuRenderer();
            ~WebGpuRenderer() override;

            WebGpuRenderer(const WebGpuRenderer&) = delete;
            WebGpuRenderer& operator=(const WebGpuRenderer&) = delete;

            /// Before Initialize. See AdapterPreference
            void SetAdapterPreference(AdapterPreference preference);

            /// Creates the instance, adapter, device, queue and an offscreen target of width x height (both non-zero; the
            /// window handle is accepted and not used). Returns false and leaves nothing allocated on any failure; see
            /// GetInitStatus. Initializing a renderer that is already initialized shuts it down first (a new renderer for
            /// everything holding the old one's lifetime token).
            bool Initialize(GLFWwindow* windowHandle, uint32_t width, uint32_t height) override;
            /// Destroys every WebGPU object, the device last but one, and expires the lifetime token. Safe to call twice, or
            /// without Initialize
            void Shutdown() override;
            /// Sets the size of the offscreen target, unless SetRenderTargetSize fixed it (then ignored)
            void Resize(uint32_t width, uint32_t height) override;
            /// The colour the next BeginFrame clears to; between BeginFrame and EndFrame it also clears the frame so far
            /// (colour and depth)
            void Clear(float r, float g, float b, float a) override;

            void BeginFrame() override;
            void EndFrame() override;
            /// Nothing to present (no swapchain, no window): logs a warning to stderr the first time, once per renderer
            void Present() override;

            // Not implemented until G3a/G3b: nullptr / false / no-op, one warning per renderer and call
            Common::IShader* CreateShaderProgram(const char* vertexSource, const char* fragmentSource) override;
            void UseShaderProgram(Common::IShader* shader) override;
            bool DestroyShaderProgram(Common::IShader* shader) override;
            bool IsValidShader(Common::IShader* shader) const override;
            Common::IMesh* CreateMesh(const Common::MeshData& meshData) override;
            void DestroyMesh(Common::IMesh* mesh) override;
            Common::ITexture*
            CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels) override;
            Common::ITexture* CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels,
                                            const Common::TextureOptions& options) override;
            void DestroyTexture(Common::ITexture* texture) override;
            Common::IMaterial* CreateMaterial(Common::IShader* shader) override;
            Common::IMaterial* CreateMaterial(Common::IShader* shader, Common::ITexture* texture) override;
            void DestroyMaterial(Common::IMaterial* material) override;
            bool UpdateMesh(Common::IMesh* mesh, const Common::MeshData& meshData) override;
            /// Ignored (the view and projection matter once there are draws)
            void SetViewProjection(const float* view, const float* projection) override;
            /// Ignored (lighting matters once there are lit draws)
            void UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                     const N2Engine::Math::Vector3& cameraPosition) override;
            using Common::IRenderer::DrawMesh; // the default-state overload
            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                          const Common::RenderState& state) override;
            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                          const Common::RenderState& state, const Common::IndexRange& range) override;
            void DrawObjects(const std::vector<Common::RenderObject>& objects) override;
            /// Like Resize, from a window's framebuffer-size callback
            void OnResize(int width, int height) override;
            void SetWireframe(bool enabled) override;
            [[nodiscard]] const char* GetRendererName() const override { return "WebGPU Renderer"; }
            /// nullptr until G3a (unlit) and G3b (lit, text)
            [[nodiscard]] Common::IShader* GetStandardUnlitShader() const override;
            [[nodiscard]] Common::IShader* GetStandardLitShader() const override;
            [[nodiscard]] Common::IShader* GetStandardTextShader() const override;

            /// RGBA8, bottom row first, of the last frame (all zero bytes before the first), read SYNCHRONOUSLY: the colour
            /// target is copied into a buffer whose rows are padded to 256 bytes, mapped (this call waits for the map for up
            /// to 10 seconds) and unpadded. A size other than the target's is resampled, nearest, as the software and Vulkan
            /// renderers do. Writes nothing when not initialized, when the device is lost, when there is no target, or when
            /// the copy or the wait fails (a message on stderr).
            void ReadFramebuffer(std::uint8_t* buffer, int width, int height) const override;
            /// Recreates the offscreen colour and depth targets at width x height and keeps them at that size (Resize and
            /// OnResize then do nothing). False for a zero dimension (nothing changes), a size over the device's texture or
            /// buffer limits, an uninitialized or lost device, or an allocation failure (then the old target is gone and
            /// frames are skipped until a later call succeeds). The same size again keeps the target.
            [[nodiscard]] bool SetRenderTargetSize(uint32_t width, uint32_t height) override;

            // State for tests and diagnostics
            [[nodiscard]] InitStatus GetInitStatus() const;
            /// The reason of the last failed Initialize, or empty
            [[nodiscard]] std::string GetInitMessage() const;
            /// The adapter's device name and description, empty before a successful Initialize
            [[nodiscard]] std::string GetAdapterName() const;
            [[nodiscard]] WGPUAdapterType GetAdapterType() const;
            [[nodiscard]] WGPUBackendType GetBackendType() const;
            /// The size of the offscreen target; 0 x 0 when there is none
            [[nodiscard]] uint32_t GetRenderWidth() const;
            [[nodiscard]] uint32_t GetRenderHeight() const;
            [[nodiscard]] static constexpr WGPUTextureFormat GetColorFormat() { return WGPUTextureFormat_RGBA8Unorm; }
            [[nodiscard]] static constexpr WGPUTextureFormat GetDepthFormat() { return WGPUTextureFormat_Depth32Float; }

            /// The validation-layer analogue: how many errors the device has reported since this renderer OBJECT was
            /// created (not reset by Shutdown or Initialize, so one number covers a whole test). Every uncaptured error
            /// (validation, out of memory, internal, unknown) counts, and so does a device loss the renderer did not cause
            /// (anything but its own Shutdown destroying the device). Errors reach the callback asynchronously: this call
            /// first makes the device deliver them (processes events and waits for the queue to go idle), so an error
            /// caused by an earlier call is counted when this returns.
            [[nodiscard]] uint32_t GetValidationErrorCount() const;
            /// The first messages of those errors (type and text), up to 32
            [[nodiscard]] std::vector<std::string> GetValidationMessages() const;
            /// How many device losses counted as errors (a subset of GetValidationErrorCount)
            [[nodiscard]] uint32_t GetDeviceLostCount() const;
            /// True once the device was lost (not by Shutdown): frames are skipped, nothing is read back
            [[nodiscard]] bool IsDeviceLost() const;
            /// Zeroes the counters and forgets the stored messages (tests that provoke an error on purpose)
            void ResetValidationErrors();

            // Test hooks. The handles are borrowed: the renderer owns and releases them, so a test must not release them,
            // and they are null when there is no device / target. The raw device is how a test provokes an error on purpose
            [[nodiscard]] WGPUInstance GetInstanceForTest() const;
            [[nodiscard]] WGPUDevice GetDeviceForTest() const;
            [[nodiscard]] WGPUQueue GetQueueForTest() const;
            /// The colour target (RGBA8Unorm; usages RenderAttachment, CopySrc and CopyDst, so a test can write a pattern into it)
            [[nodiscard]] WGPUTexture GetColorTextureForTest() const;
            [[nodiscard]] WGPUTexture GetDepthTextureForTest() const;
            /// The depth target's last frame as width * height floats, row 0 the top of the image. False when there is no target
            /// or the read fails
            bool ReadDepthForTest(std::vector<float>& depth, uint32_t* width = nullptr, uint32_t* height = nullptr) const;
            /// The device's limits for texture dimension and buffer size (0 before Initialize)
            [[nodiscard]] uint32_t GetMaxTextureDimensionForTest() const;
            [[nodiscard]] uint64_t GetMaxBufferSizeForTest() const;
            /// How many WebGPU objects (instances, adapters, devices, queues, textures, views, buffers, command encoders and
            /// buffers, passes) this process holds a reference to through a WebGpuRenderer right now, across all renderers.
            /// It returns to its baseline once every renderer is shut down: a count that does not is a leaked reference
            [[nodiscard]] static std::int64_t GetLiveObjectCountForTest();

        private:
            struct State; // the WebGPU handles and the error counters (private, in the .cpp)

            // Everything below with "Locked" in a comment runs with _mutex held
            bool Fail(InitStatus status, const std::string& message);
            void ShutdownLocked();
            bool CreateInstance();
            bool PickAdapter();
            bool CreateDevice();
            bool CreateTargets(uint32_t width, uint32_t height);
            void DestroyTargets();
            bool RecreateTargets(uint32_t width, uint32_t height);
            bool BeginPass();
            void EndPass();
            void AbandonOpenFrame();
            /// Makes the device deliver the errors it holds (events, then a wait for the queue), then returns
            void FlushLocked() const;
            /// Copies `texture` (all of it, one aspect) into a temporary buffer with 256-byte aligned rows and returns it
            /// tightly packed. False on any failure (logged)
            bool CopyTextureToHostLocked(WGPUTexture texture, WGPUTextureAspect aspect, uint32_t bytesPerPixel,
                                         uint32_t width, uint32_t height, std::vector<std::uint8_t>& pixels) const;
            void LogUnsupportedOnce(const char* what) const;
            void LogOnce(const std::string& message) const;

            mutable std::mutex _mutex;
            mutable std::mutex _logMutex;
            mutable std::set<std::string> _logged;

            AdapterPreference _preference = AdapterPreference::Default;
            std::shared_ptr<State> _state; // never null; the counters outlive Shutdown and Initialize
            InitStatus _initStatus = InitStatus::NotInitialized;
            std::string _initMessage;
            bool _initialized = false;
            bool _frameOpen = false;
            bool _warnedPresent = false;

            uint32_t _targetWidth = 0;
            uint32_t _targetHeight = 0;
            bool _targetFixed = false; // SetRenderTargetSize set the size: Resize and OnResize are ignored
            float _clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        };
    }
}
