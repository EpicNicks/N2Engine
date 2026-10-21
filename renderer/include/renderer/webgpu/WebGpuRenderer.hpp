#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Renderer.hpp first: it includes glad, which must come before GLFW's header (that one pulls in GL itself otherwise)
#include "renderer/common/Renderer.hpp"
#include "renderer/common/GpuShaderInterface.hpp"

// The C API only (webgpu.h, as Dawn ships it): no Dawn C++ header (webgpu_cpp.h, dawn/native/*) may be included from
// here or from renderer/src/webgpu/, because stage G4 compiles the same files with Emscripten's emdawnwebgpu port.
#include <webgpu/webgpu.h>

namespace Renderer
{
    namespace WebGpu
    {
        // The shader interface table is shared by the GPU backends (renderer/common/GpuShaderInterface.hpp)
        namespace ShaderInterface = Gpu::ShaderInterface;

        class WebGpuTexture; // a texture (private header)

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

        /// What a test can ask about a mesh (GetMeshInfoForTest); `valid` is false for a pointer this renderer did not make (or
        /// already destroyed). The buffers are borrowed: the mesh owns them
        struct MeshInfoForTest
        {
            bool valid = false;
            WGPUBuffer vertexBuffer = nullptr;
            WGPUBuffer indexBuffer = nullptr; // null for a mesh with no indices
            uint32_t vertexCount = 0;
            uint32_t indexCount = 0;
        };

        /// What a test can ask about a texture (GetTextureInfoForTest); `valid` is false for a pointer this renderer did not
        /// make (or already destroyed)
        struct TextureInfoForTest
        {
            bool valid = false;
            WGPUTextureFormat format = WGPUTextureFormat_Undefined; // 3 channels are stored as RGBA8
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t channels = 0; // as given to CreateTexture
            uint32_t mipLevels = 0;
            WGPUFilterMode filter = WGPUFilterMode_Nearest;                     // min and mag filter of the sampler
            WGPUMipmapFilterMode mipmapFilter = WGPUMipmapFilterMode_Nearest;
            WGPUAddressMode addressMode = WGPUAddressMode_Repeat;               // U, V and W
            WGPUSampler sampler = nullptr; // the cached sampler (shared between textures with equal options); borrowed
        };

        /**
         * The WebGPU backend, stages G2, G3a and G3b of #155: a renderer that runs on Dawn (natively, for the CI harness only).
         * Headless initialisation (instance, adapter, device, queue), an offscreen RGBA8 colour target and a Depth32Float
         * depth target, a frame that clears them, a synchronous ReadFramebuffer, SetRenderTargetSize and Shutdown, plus
         * counting of every error the device reports (stage G2); meshes (with the in-frame UpdateMesh rule), textures
         * (RGBA8 or R8/RG8, sRGB, samplers, CPU-generated mipmaps), materials, a pipeline cache, a bind-group cache, a
         * per-draw uniform ring and the unlit shader with DrawMesh, IndexRange and DrawObjects (stage G3a). There is no window
         * and no swapchain: Present only warns.
         *
         * What draws (stage G3b completed it): everything the software renderer draws, through the three built-in shaders: UNLIT, LIT
         * (aux textures, Blinn-Phong or PBR, the scene lighting of UpdateSceneLighting) and TEXT (the SDF shader with outline,
         * shadow and softness effects), with the depth, depth write, cull and blend state of every RenderState (blend and
         * additive blending as OpenGL), and a SetViewProjection that may change between draws of one frame. NOT supported:
         * custom shaders (CreateShaderProgram returns nullptr, as on the other GPU backends), wireframe (not in core WebGPU:
         * SetWireframe is ignored with one log line, design decision W9) and compute. Stage G4 compiles the backend for the
         * browser.
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

            /// Custom shaders are not supported in v1: logs one warning and returns nullptr. The shaders are the built-ins
            Common::IShader* CreateShaderProgram(const char* vertexSource, const char* fragmentSource) override;
            /// A no-op: a draw takes its shader from its material
            void UseShaderProgram(Common::IShader* shader) override;
            /// Always false: the built-in shaders belong to the renderer and are never destroyed by a caller
            bool DestroyShaderProgram(Common::IShader* shader) override;
            /// True only for this renderer's built-in shaders (unlit, lit, text) once initialized
            bool IsValidShader(Common::IShader* shader) const override;

            // Meshes and textures. Uploads are queue writes (they work inside or outside a frame). Destroying a mesh or a
            // texture only releases it: WebGPU keeps an object alive for every recorded or submitted command that uses it.
            /// A vertex buffer (64-byte Vertex) and a uint32 index buffer. nullptr for no vertices, for data over the device's
            /// buffer limit, or when the renderer is not initialized or the device is lost. A mesh with vertices and no
            /// indices has an index count of 0 and no index buffer (it draws nothing)
            Common::IMesh* CreateMesh(const Common::MeshData& meshData) override;
            /// Ignores nullptr and any pointer this renderer did not make (another renderer's, another backend's, a fake)
            void DestroyMesh(Common::IMesh* mesh) override;
            /// CreateTexture with TextureOptions::Default()
            Common::ITexture*
            CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels) override;
            /// 1 channel: R8Unorm (samples (r,0,0,1)); 2: RG8Unorm ((r,g,0,1)); 3: expanded to RGBA8 with alpha 255; 4: RGBA8.
            /// srgb picks RGBA8UnormSrgb for 3 and 4 channels (1 and 2 channels ignore it). Rows are tightly packed. With
            /// options.mipmaps a full mip chain, generated on the CPU (a box filter, in linear light for sRGB textures: core
            /// WebGPU has no blit), else one level. nullptr for null data, a zero size, channels outside 1-4, a size over the
            /// device's limit, a lost device or an uninitialized renderer
            Common::ITexture* CreateTexture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t channels,
                                            const Common::TextureOptions& options) override;
            /// Ignores nullptr and pointers this renderer did not make, like DestroyMesh
            void DestroyTexture(Common::ITexture* texture) override;
            /// A material of a built-in shader (unlit, lit or text): the OpenGL defaults (uAlbedo white,
            /// uHasTexture 1 with a texture, uAlphaCutoff 0), values set by name (the names of the shader's material block; any
            /// other name, or the wrong type, is logged once and ignored), the base texture. DrawMesh takes a snapshot of its
            /// values and textures, so a change after a draw does not affect that draw. nullptr for a null shader or one this
            /// renderer did not make
            Common::IMaterial* CreateMaterial(Common::IShader* shader) override;
            Common::IMaterial* CreateMaterial(Common::IShader* shader, Common::ITexture* texture) override;
            /// Ignores nullptr and any pointer this renderer did not make; Shutdown frees the rest
            void DestroyMaterial(Common::IMaterial* material) override;
            /// Replaces the mesh's vertices and indices, keeping the same IMesh*. The IN-FRAME RULE: a queue write runs on the
            /// queue's timeline at once, before the open frame's commands are submitted, so a mesh already drawn in the frame
            /// being recorded gets NEW buffers (the earlier draws keep the old ones and read the old vertices). A mesh not yet
            /// drawn in this frame, or any mesh outside a frame, is written in place when the vertex and index counts are
            /// unchanged, and gets new buffers otherwise. False, with the mesh unchanged, for nullptr, a pointer this renderer
            /// did not make, data with no vertices, or a lost device
            bool UpdateMesh(Common::IMesh* mesh, const Common::MeshData& meshData) override;
            /// The row-major view and projection of the draws that follow (until the next call): each draw copies the values
            /// current when it is made. Null arguments are ignored. Identity until set
            void SetViewProjection(const float* view, const float* projection) override;
            /// Builds the lit shader's lighting block (GpuLighting.hpp) into a CPU copy; the first lit draw after this copies it
            /// into the frame's ring, so a draw uses the lighting set by the last call BEFORE it (an update after a draw does not
            /// change that draw), as on OpenGL and Vulkan
            void UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                     const N2Engine::Math::Vector3& cameraPosition) override;
            using Common::IRenderer::DrawMesh; // the default-state overload
            /// Draws the whole index buffer with the material's shader and this draw's depth test, depth write, cull and blend
            /// state (blend: SRC_ALPHA / ONE_MINUS_SRC_ALPHA, additive: SRC_ALPHA / ONE, for colour and alpha, as OpenGL). Recorded into the open frame, in submission
            /// order, taking a snapshot of the material's values and of the view and projection. Ignored (nothing drawn, no
            /// error) outside BeginFrame..EndFrame, for a null or foreign mesh, material or model matrix, and for a mesh
            /// without indices
            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                          const Common::RenderState& state) override;
            /// Draws the indices [range.first, range.first + range.count), as OpenGL and Vulkan do: NOTHING for a count of 0 or
            /// a range that is not inside the mesh's indices (the software renderer clips such a range to the mesh instead)
            void DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                          const Common::RenderState& state, const Common::IndexRange& range) override;
            /// DrawMesh of each object's mesh, transform.model, material and state, in order
            void DrawObjects(const std::vector<Common::RenderObject>& objects) override;
            /// Like Resize, from a window's framebuffer-size callback
            void OnResize(int width, int height) override;
            /// Not in core WebGPU (no polygon mode): ignored, one log line
            void SetWireframe(bool enabled) override;
            [[nodiscard]] const char* GetRendererName() const override { return "WebGPU Renderer"; }
            /// The three built-ins; nullptr before Initialize
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

            struct State; // the WebGPU handles, the error counters and the resources (opaque here; defined in a private header)

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
            /// Lowers the buffer-size limit the renderer checks a readback against, so a test can reach that check
            void SetMaxBufferSizeForTest(uint64_t bytes);
            /// How many WebGPU objects (instances, adapters, devices, queues, textures, views, buffers, command encoders and
            /// buffers, passes) this process holds a reference to through a WebGpuRenderer right now, across all renderers.
            /// It returns to its baseline once every renderer is shut down: a count that does not is a leaked reference
            [[nodiscard]] static std::int64_t GetLiveObjectCountForTest();

            // Test hooks for G3a
            /// The buffers and counts of a mesh this renderer made
            [[nodiscard]] MeshInfoForTest GetMeshInfoForTest(const Common::IMesh* mesh) const;
            /// Copies a mesh's vertex and index buffers back. False for a pointer that is not this renderer's
            bool ReadMeshForTest(const Common::IMesh* mesh, Common::MeshData& out) const;
            /// Format, size, mip count and sampler of a texture this renderer made
            [[nodiscard]] TextureInfoForTest GetTextureInfoForTest(const Common::ITexture* texture) const;
            /// Copies mip level `mipLevel` of a texture into `bytes`, tightly packed in the texture's format (RGBA8 for 3 and 4
            /// channels, R8, RG8). False for a pointer that is not this renderer's or a level that does not exist
            bool ReadTextureForTest(const Common::ITexture* texture, uint32_t mipLevel, std::vector<uint8_t>& bytes,
                                    uint32_t* levelWidth = nullptr, uint32_t* levelHeight = nullptr) const;
            /// The id of a live texture (unique for the renderer's life, never reused); 0 for any other pointer
            [[nodiscard]] uint64_t GetTextureIdForTest(const Common::ITexture* texture) const;
            [[nodiscard]] uint32_t GetLiveMeshCountForTest() const;
            /// Textures made through CreateTexture (the renderer's own 1x1 defaults are not counted)
            [[nodiscard]] uint32_t GetLiveTextureCountForTest() const;
            [[nodiscard]] uint32_t GetLiveMaterialCountForTest() const;
            /// The pipelines made since the last Initialize (one per distinct (shader, vertex layout, RenderState) used)
            [[nodiscard]] uint32_t GetPipelineCountForTest() const;
            /// How many times a pipeline was actually attempted since the last Initialize (a cache hit or a remembered failure
            /// is not an attempt)
            [[nodiscard]] uint32_t GetPipelineCreateAttemptCountForTest() const;
            /// Makes (or finds) the pipeline for (shader, state) now, as a draw would. False when there is no such shader
            bool CreatePipelineForTest(ShaderInterface::BuiltInShader shader, const Common::RenderState& state);
            /// Makes every pipeline creation fail (without calling the device) while set
            void SetFailPipelineCreationForTest(bool fail);
            /// The material bind groups cached (one per distinct (ring chunk, five textures) drawn with)
            [[nodiscard]] uint32_t GetBindGroupCountForTest() const;
            /// The uniform ring buffers the renderer holds (they grow when a frame needs more room and are kept)
            [[nodiscard]] uint32_t GetRingChunkCountForTest() const;
            /// The size a NEW ring chunk gets (at least what one draw needs), so a test can reach the chunk-growth path
            void SetRingChunkBytesForTest(uint64_t bytes);
            /// The CPU copy of the lighting block (kLightingBlockSize bytes) as the next lit draw would copy it into the ring:
            /// what the last UpdateSceneLighting built, or SceneLightingData{} seen from the origin before the first
            [[nodiscard]] std::vector<uint8_t> GetLightingBlockForTest() const;
            /// The device's minUniformBufferOffsetAlignment, which every per-draw offset is a multiple of
            [[nodiscard]] uint32_t GetUniformAlignmentForTest() const;
            /// A copy of the material's std140 block (kMaterialBlockRange bytes: the shader's block, then zeros) as a draw would
            /// snapshot it now; false for a material that is not this renderer's
            bool GetMaterialBlockForTest(const Common::IMaterial* material, std::vector<uint8_t>& bytes) const;
            /// Replaces the texture id a material holds for a slot (0 the base texture, 1-4 the AuxTexture slots in enum order), so
            /// a test can put a stale (pointer, id) pair into a material, which is what a texture made at the address of a
            /// destroyed one looks like to the draw. False for a material that is not this renderer's or a bad slot
            bool SetMaterialTextureIdForTest(Common::IMaterial* material, std::size_t slot, std::uint64_t id);

        private:
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
            // G3a: WebGpuDraw.cpp
            bool CreateDrawResources();
            /// Releases every mesh, texture, material, pipeline, bind group and ring chunk (device-free: handles only)
            void DestroyAllResources();
            void BeginDrawFrame();
            void EndDrawFrame();
            Common::IShader* GetStandardShader(ShaderInterface::BuiltInShader kind) const;
            WGPURenderPipeline GetPipelineLocked(ShaderInterface::BuiltInShader shader, const Common::RenderState& state);
            bool AddRingChunk(uint64_t minimumSize);
            bool AllocateRing(uint64_t bytes, std::size_t& chunkIndex, uint64_t& offset);
            void DrawLocked(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                            const Common::RenderState& state, uint32_t first, uint32_t count, bool wholeMesh);
            /// Makes the device deliver the errors it holds (events, then a wait for the queue), then returns
            void FlushLocked() const;
            /// Copies mip level `mipLevel` of `texture` (all of it, one aspect; width and height are the level's) into a
            /// temporary buffer with 256-byte aligned rows and returns it tightly packed. False on any failure (logged)
            bool CopyTextureToHostLocked(WGPUTexture texture, WGPUTextureAspect aspect, uint32_t bytesPerPixel,
                                         uint32_t width, uint32_t height, std::vector<std::uint8_t>& pixels,
                                         uint32_t mipLevel = 0) const;
            /// Copies `size` bytes of `buffer` (which needs CopySrc) into `bytes`. False on any failure (logged)
            bool CopyBufferToHostLocked(WGPUBuffer buffer, uint64_t size, std::vector<std::uint8_t>& bytes) const;
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
