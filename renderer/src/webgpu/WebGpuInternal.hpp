#pragma once

// PRIVATE to renderer/src/webgpu/: the WebGPU backend's handle wrappers, error counters, resource classes and the renderer's
// State (stages G2, G3a and G3b of #155), shared by WebGpuRenderer.cpp (core: device, targets, frames, readback),
// WebGpuResources.cpp (meshes, textures, samplers) and WebGpuDraw.cpp (shaders, materials, pipelines, the uniform ring, draws).
// Never included from a public header.
//
// Written against webgpu.h (the C API) only: nothing here may name a Dawn C++ type, because stage G4 compiles these files with
// Emscripten's emdawnwebgpu port.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/common/BlockMaterial.hpp"
#include "renderer/common/GpuLighting.hpp"
#include "renderer/common/GpuShaderInterface.hpp"
#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IMesh.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/common/RenderTypes.hpp"
#include "renderer/common/TextureOptions.hpp"
#include "renderer/webgpu/WebGpuRenderer.hpp"

namespace Renderer::WebGpu
{
    inline constexpr std::uint32_t RowAlignment = 256; // bytesPerRow of a texture <-> buffer copy

    inline std::string ToString(WGPUStringView view)
    {
        if (view.data == nullptr)
        {
            return {};
        }
        return view.length == WGPU_STRLEN ? std::string(view.data) : std::string(view.data, view.length);
    }

    inline WGPUStringView ToView(const char* text)
    {
        return WGPUStringView{text, std::strlen(text)};
    }

    inline std::uint32_t AlignUp(std::uint32_t value, std::uint32_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    inline std::uint64_t AlignUp64(std::uint64_t value, std::uint64_t alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    /// WebGPU references this process holds through a WebGpuRenderer
    inline std::atomic<std::int64_t> g_liveObjects{0};

    /// One reference to a WebGPU object, released on destruction or Reset; counted in g_liveObjects
    template <typename Handle, void (*ReleaseFn)(Handle)>
    class Ref
    {
    public:
        Ref() = default;
        explicit Ref(Handle handle) : _handle(handle)
        {
            if (_handle != nullptr)
            {
                ++g_liveObjects;
            }
        }
        Ref(const Ref&) = delete;
        Ref& operator=(const Ref&) = delete;
        Ref(Ref&& other) noexcept : _handle(other._handle) { other._handle = nullptr; }
        Ref& operator=(Ref&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                _handle = other._handle;
                other._handle = nullptr;
            }
            return *this;
        }
        ~Ref() { Reset(); }

        void Reset(Handle handle = nullptr)
        {
            if (_handle != nullptr)
            {
                ReleaseFn(_handle);
                --g_liveObjects;
            }
            _handle = handle;
            if (_handle != nullptr)
            {
                ++g_liveObjects;
            }
        }
        [[nodiscard]] Handle Get() const { return _handle; }
        explicit operator bool() const { return _handle != nullptr; }

    private:
        Handle _handle = nullptr;
    };

    using InstanceRef = Ref<WGPUInstance, wgpuInstanceRelease>;
    using AdapterRef = Ref<WGPUAdapter, wgpuAdapterRelease>;
    using DeviceRef = Ref<WGPUDevice, wgpuDeviceRelease>;
    using QueueRef = Ref<WGPUQueue, wgpuQueueRelease>;
    using TextureRef = Ref<WGPUTexture, wgpuTextureRelease>;
    using TextureViewRef = Ref<WGPUTextureView, wgpuTextureViewRelease>;
    using BufferRef = Ref<WGPUBuffer, wgpuBufferRelease>;
    using SamplerRef = Ref<WGPUSampler, wgpuSamplerRelease>;
    using ShaderModuleRef = Ref<WGPUShaderModule, wgpuShaderModuleRelease>;
    using BindGroupLayoutRef = Ref<WGPUBindGroupLayout, wgpuBindGroupLayoutRelease>;
    using BindGroupRef = Ref<WGPUBindGroup, wgpuBindGroupRelease>;
    using PipelineLayoutRef = Ref<WGPUPipelineLayout, wgpuPipelineLayoutRelease>;
    using RenderPipelineRef = Ref<WGPURenderPipeline, wgpuRenderPipelineRelease>;
    using CommandEncoderRef = Ref<WGPUCommandEncoder, wgpuCommandEncoderRelease>;
    using CommandBufferRef = Ref<WGPUCommandBuffer, wgpuCommandBufferRelease>;
    using RenderPassRef = Ref<WGPURenderPassEncoder, wgpuRenderPassEncoderRelease>;

    /// What the device's two callbacks count into. They may run on any thread, inside any WebGPU call that processes
    /// events: they take only this block's own mutex, never the renderer's
    struct ErrorCounters
    {
        std::mutex mutex;
        std::uint32_t errors = 0;     // every uncaptured error and every unexpected loss
        std::uint32_t lostCount = 0;  // the losses among them
        std::vector<std::string> messages;
        std::atomic<bool> lost{false};         // the device was lost and the renderer did not cause it
        std::atomic<bool> expectedLoss{false}; // the renderer is destroying its device: the loss is not an error
    };

    /// What a buffer map's callback (wgpuBufferMapAsync) reports. Natively a readback waits for it; in the browser (stage G4) the
    /// asynchronous ReadFramebuffer keeps it with the copy in flight and looks at it on its next call
    struct MapResult
    {
        bool done = false;
        WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
        std::string message;
    };

    // -----------------------------------------------------------------------------------------------------------------
    // Resources (G3a)
    // -----------------------------------------------------------------------------------------------------------------

    /// A mesh: a vertex buffer of Common::Vertex (64 bytes each) and a uint32 index buffer. The index buffer is null
    /// (indexCount 0) for a mesh whose data had vertices but no indices. UpdateMesh may replace the buffers, so a draw reads
    /// them under the renderer's lock. Destroying or replacing a buffer only RELEASES it: WebGPU keeps an object alive for
    /// every recorded or submitted command that uses it, so nothing has to be deferred
    class WebGpuMesh final : public Common::IMesh
    {
    public:
        [[nodiscard]] bool IsValid() const override { return static_cast<bool>(vertexBuffer); }
        [[nodiscard]] std::uint32_t GetIndexCount() const override { return indexCount; }
        [[nodiscard]] std::uint32_t GetVertexCount() const override { return vertexCount; }

        BufferRef vertexBuffer;
        BufferRef indexBuffer;
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        /// The frame serial of the last draw of this mesh (0: never drawn): the in-frame UpdateMesh rule compares it with the
        /// serial of the frame being recorded
        std::uint64_t lastDrawFrame = 0;
    };

    /// How a cached sampler is made; the key of the sampler cache
    struct SamplerKey
    {
        WGPUFilterMode filter = WGPUFilterMode_Linear;
        WGPUMipmapFilterMode mipmapFilter = WGPUMipmapFilterMode_Linear;
        WGPUAddressMode addressMode = WGPUAddressMode_Repeat;

        friend bool operator==(const SamplerKey&, const SamplerKey&) = default;
    };

    /// A texture: all its mip levels written, a view of all of them, and the (cached, renderer-owned) sampler for its options
    class WebGpuTexture final : public Common::ITexture
    {
    public:
        [[nodiscard]] bool IsValid() const override { return static_cast<bool>(texture); }
        [[nodiscard]] std::uint32_t GetWidth() const override { return width; }
        [[nodiscard]] std::uint32_t GetHeight() const override { return height; }
        [[nodiscard]] std::uint32_t GetChannels() const override { return channels; }

        TextureRef texture;
        TextureViewRef view;
        WGPUSampler sampler = nullptr; // owned by the sampler cache
        SamplerKey samplerKey;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t channels = 0; // as the caller gave them (a 3-channel texture is stored as RGBA8)
        std::uint32_t mipLevels = 1;
        std::uint64_t id = 0; // from a counter that never repeats (State::nextTextureId)
        Common::TextureOptions options;
    };

    // -----------------------------------------------------------------------------------------------------------------
    // Drawing (G3a)
    // -----------------------------------------------------------------------------------------------------------------

    /// One of the built-in shaders (unlit, lit, text). The two entry points of a built-in live in ONE WGSL module (vs_main,
    /// fs_main). There is no custom WGSL in v1, so LoadFromStrings fails and the Set* calls (a shader holds no values, the
    /// material does) do nothing
    class WebGpuShader final : public Common::IShader
    {
    public:
        explicit WebGpuShader(const ShaderInterface::BuiltInShader shaderKind) : kind(shaderKind) {}

        bool LoadFromStrings(const std::string&, const std::string&) override { return false; }
        void Bind() const override {}
        void Unbind() const override {}
        bool IsValid() const override { return static_cast<bool>(module); }

        void SetFloat(const std::string&, float) override {}
        void SetInt(const std::string&, int) override {}
        void SetBool(const std::string&, bool) override {}
        void SetVec2(const std::string&, const N2Engine::Math::Vector2&) override {}
        void SetVec3(const std::string&, const N2Engine::Math::Vector3&) override {}
        void SetVec4(const std::string&, const N2Engine::Math::Vector4&) override {}
        void SetMat4(const std::string&, const N2Engine::Math::Matrix<float, 4, 4>&) override {}
        void SetVec2(const std::string&, float, float) override {}
        void SetVec3(const std::string&, float, float, float) override {}
        void SetVec4(const std::string&, float, float, float, float) override {}

        ShaderInterface::BuiltInShader kind;
        ShaderModuleRef module;
    };

    /// The pipeline cache key: (shader, vertex layout, RenderState) packed. The vertex layout is the attribute set of the
    /// shader (ShaderInterface::Attributes), so today it follows from the shader; it is a field of its own so a second layout
    /// (a shader reading other attributes, or a second vertex buffer) can never share a pipeline with the first.
    /// blend and additive select the colour target's blend state (G3b): opaque, alpha blend or additive
    struct PipelineKey
    {
        std::uint8_t shader = 0;
        std::uint8_t vertexLayout = 0;
        bool depthTest = true;
        bool depthWrite = true;
        std::uint8_t cull = 0;
        bool blend = false;
        bool additive = false;

        [[nodiscard]] std::uint32_t Pack() const
        {
            return static_cast<std::uint32_t>(shader) | (static_cast<std::uint32_t>(vertexLayout) << 4) |
                (depthTest ? 1u << 8 : 0u) | (depthWrite ? 1u << 9 : 0u) | (static_cast<std::uint32_t>(cull) << 10) |
                (blend ? 1u << 12 : 0u) | (additive ? 1u << 13 : 0u);
        }
    };

    /// A piece of the per-draw uniform ring: a uniform buffer plus the CPU copy the draws of a frame fill from the start;
    /// EndFrame writes the used part to the buffer with one queue write before the submit. When a chunk is full the frame
    /// moves on to the next (a new one, at least as big as the draw needs), so any number of draws works
    struct RingChunk
    {
        BufferRef buffer;
        std::vector<std::uint8_t> staging;
        std::uint64_t size = 0;
        std::uint64_t used = 0;
        BindGroupRef frameGroup; // group 0 for this chunk: the per-draw block and the lighting block, each at a dynamic offset
        // The copy of the scene lighting block in this chunk this frame (G3b): which State::lightingVersion it holds (0: none
        // yet this frame) and where. A lit draw names the copy by this offset; an unlit or text draw names offset 0 (in range,
        // never read)
        std::uint64_t lightingVersion = 0;
        std::uint64_t lightingOffset = 0;
    };

    /// The renderer's number of built-in shaders and of material textures (base + the four AuxTexture slots)
    inline constexpr std::size_t kShaderCount = ShaderInterface::kBuiltInShaderCount;
    inline constexpr std::size_t kTextureSlots = ShaderInterface::kSamplerCount;

    /// The WebGPU handles, the error counters the device's callbacks reach through a raw pointer (the block is the callbacks'
    /// userdata, so its address must stay put: it lives as long as the renderer), and everything G3a adds
    struct WebGpuRenderer::State : ErrorCounters
    {
        InstanceRef instance;
        AdapterRef adapter;
        DeviceRef device;
        QueueRef queue;
        TextureRef color;
        TextureRef depth;
        TextureViewRef colorView;
        TextureViewRef depthView;
        CommandEncoderRef encoder; // the open frame's, null between frames
        RenderPassRef pass;        // the open frame's render pass, null between frames

        std::string adapterName;
        WGPUAdapterType adapterType = WGPUAdapterType_Unknown;
        WGPUBackendType backendType = WGPUBackendType_Undefined;
        std::uint32_t maxTextureDimension = 0;
        std::uint64_t maxBufferSize = 0;
        std::uint32_t uniformAlignment = 256; // minUniformBufferOffsetAlignment

        // ---- G3a: resources. The registries own the CPU objects; a pointer that is not a key is not this renderer's, and
        // foreign pointers are only ever compared, never dereferenced
        std::unordered_map<const Common::IMesh*, std::unique_ptr<WebGpuMesh>> meshes;
        std::unordered_map<const Common::ITexture*, std::unique_ptr<WebGpuTexture>> textures;
        std::vector<std::pair<SamplerKey, SamplerRef>> samplers;
        std::unordered_map<const Common::IMaterial*, std::unique_ptr<Gpu::BlockMaterial>> materials;

        // Texture ids: a counter that never repeats, so a texture made at the address of a destroyed one is a different
        // texture. The map is its own leaf lock (never held with another lock taken inside it) so a material can ask for an id
        // from any thread
        std::mutex textureIdMutex;
        std::unordered_map<const Common::ITexture*, std::uint64_t> textureIds;
        std::uint64_t nextTextureId = 1;

        // ---- G3a: drawing
        std::array<std::unique_ptr<WebGpuShader>, kShaderCount> shaders;
        BindGroupLayoutRef frameLayout;    // group 0: binding 0 the per-draw block, binding 1 the lighting block (both dynamic)
        BindGroupLayoutRef materialLayout; // group 1: binding 0 the material block (dynamic), 1-5 textures, 6-10 samplers
        PipelineLayoutRef pipelineLayout;
        std::map<std::uint32_t, RenderPipelineRef> pipelines;
        // Keys whose pipeline could not be made: remembered so a failure is logged once and later draws are skipped
        std::set<std::uint32_t> failedPipelines;
        std::uint32_t pipelineCreateAttempts = 0;
        bool failPipelineCreationForTest = false;
        // Group 1 by (ring chunk, the five texture ids): immutable, so shared by every draw with the same textures in the same
        // chunk, and kept until a texture it names is destroyed (or Shutdown)
        std::map<std::array<std::uint64_t, 1 + kTextureSlots>, BindGroupRef> materialGroups;
        std::vector<RingChunk> chunks;
        std::size_t activeChunk = 0;
        std::uint64_t ringChunkBytes = 256 * 1024; // the size of a new chunk (more when one draw needs more)
        // 1x1 textures for the absent ones (base, emissive, occlusion, normal, metallic-roughness)
        std::array<std::unique_ptr<WebGpuTexture>, kTextureSlots> defaults;
        // The text shader's base sampler (its SDF atlas) defaults to (0,0,0,0) instead of white: an absent atlas reads sdf 0,
        // coverage 0 and the fragment is discarded, so atlas-less text draws nothing (as OpenGL, which binds no texture)
        std::unique_ptr<WebGpuTexture> textBaseDefault;

        // ---- G3b: the scene lighting. UpdateSceneLighting only rewrites this CPU copy and bumps the version; the first lit
        // draw after that copies it into the frame's ring (RingChunk::lightingVersion says which version a chunk holds), so a
        // draw uses the lighting set by the last UpdateSceneLighting before it. Before the first update the block holds
        // SceneLightingData{} seen from the origin, as the software renderer's members do
        Gpu::LightingBlock lighting = Gpu::BuildLightingBlock(Common::SceneLightingData{}, N2Engine::Math::Vector3(0.0f, 0.0f, 0.0f));
        std::uint64_t lightingVersion = 1; // never 0 (a chunk with 0 holds no copy)

        float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::uint64_t frameSerial = 0; // BeginFrame counts it up: the in-frame UpdateMesh rule compares against it

        // ---- G4: the browser's asynchronous ReadFramebuffer (WebGpuRenderer.cpp, AdvanceAsyncReadback); unused natively,
        // where the readback is synchronous. One copy of the colour target is in flight at a time; once its map completes the
        // next call keeps its pixels as the last completed frame
        BufferRef pendingReadback;                     // the buffer of the copy in flight (rows padded to 256 bytes), or null
        std::shared_ptr<MapResult> pendingReadbackMap; // its map's outcome, written by the map callback
        std::uint32_t pendingReadbackWidth = 0;
        std::uint32_t pendingReadbackHeight = 0;
        std::vector<std::uint8_t> lastReadback; // the last completed copy: RGBA8, tightly packed, row 0 the top of the image
        std::uint32_t lastReadbackWidth = 0;
        std::uint32_t lastReadbackHeight = 0;
    };

    /// WebGpuResources.cpp: the texture-making part of CreateTexture, without registering it (CreateTexture registers it; the
    /// default textures are kept by State::defaults). Runs with the renderer's mutex held. The texture's id is left 0 (the
    /// caller gives it one). Null on failure, logged to stderr
    std::unique_ptr<WebGpuTexture> MakeTexture(WebGpuRenderer::State& state, const std::uint8_t* data, std::uint32_t width,
                                               std::uint32_t height, std::uint32_t channels,
                                               const Common::TextureOptions& options);

    /// WebGpuResources.cpp: the cached sampler for `key`, made on first use; null on failure
    WGPUSampler FindOrCreateSampler(WebGpuRenderer::State& state, const SamplerKey& key);
}
