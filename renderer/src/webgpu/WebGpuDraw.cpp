#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "renderer/webgpu/WebGpuRenderer.hpp"

#include "WebGpuInternal.hpp"

#include <renderer/webgpu/WgslSources.hpp>

// Stages G3a and G3b of the WebGPU backend (#155): the three built-in shaders (unlit, lit, text), materials, the scene lighting,
// the pipeline cache, the bind-group cache, the per-draw uniform ring and the draws. Written against webgpu.h (the C API) only.
//
// Resource layout (the same for every built-in shader, renderer/shaders/webgpu/*.wgsl):
//   group 0 (the frame): binding 0 the per-draw block (3 matrices, 192 bytes) and binding 1 the lighting block (752 bytes, read
//   by the lit shader only), UNIFORM buffers, each with a DYNAMIC offset
//   group 1 (the material): binding 0 the material block (a dynamic offset), bindings 1-5 the base texture and the four
//   AuxTexture slots, bindings 6-10 their samplers (WGSL has no combined image sampler)
// The pipeline layout is the two groups, for all shaders, so a shader that reads fewer bindings (unlit and text do not read the
// lighting block and sample only the base texture, binding 1, with its sampler, binding 6) needs no layout of its own.
//
// The uniform ring. A draw needs a per-draw block and a material block. Each draw takes the next slot of the current ring chunk
// (a uniform buffer; when it is full the frame moves to the next chunk, a new one if there is none) and fills it in a CPU copy
// (RingChunk::staging); EndFrame writes the used part of every chunk with ONE queue write per chunk, just before the submit.
// A slot is aligned to the device's minUniformBufferOffsetAlignment (a dynamic offset must be a multiple of it), so no draw of
// a frame overwrites another's block, and the next frame may reuse the whole chunk: its writes come after this frame's submit
// on the queue's timeline, so this frame has read its blocks by then. Nothing is mapped and nothing waits.
//
// SCENE LIGHTING (G3b). UpdateSceneLighting builds the 752-byte std140 block (GpuLighting.hpp) into a CPU copy and bumps a
// version; it touches no GPU memory. The first LIT draw after an update copies the block into the ring (after its material
// block) and every lit draw names that copy with the dynamic offset of group 0's binding 1: one copy per ring chunk per version.
// So, as on OpenGL, a draw uses the lighting set by the last UpdateSceneLighting BEFORE it, and an update after a draw does not
// change it. A draw that does not read the lighting (unlit, text) names the chunk's copy if it has one, else offset 0, which is in
// range (a chunk is never smaller than the block) and is never read.
//
// Matrices. WGSL has only column-major matrices; the engine's are row-major. Each matrix is TRANSPOSED once, when the draw
// fills its per-draw block (WriteTransposed): the bytes become the matrix's columns, which is what mat4x4<f32> reads, and the
// WGSL multiplies exactly as the GLSL does. Nothing else transposes, and the WGSL does not.
//
// Bind groups are immutable. The frame group is made once per ring chunk. The material group is cached by (ring chunk, the five
// texture ids) and kept until a texture it names is destroyed: draws with the same textures in the same chunk share one.
//
// Pipelines are cached by (shader, vertex layout, depth test, depth write, cull, blend, additive). Depth: compare LESS (GL's
// default) with the test on and ALWAYS with it off, depth written only while testing; cull: counter-clockwise front faces, as GL.
// Blending, exactly as OpenGL's ApplyRenderState and the Vulkan backend: blend -> SRC_ALPHA / ONE_MINUS_SRC_ALPHA (additive:
// SRC_ALPHA / ONE) for colour AND alpha with equation ADD, so the target's alpha is blended too; additive only counts with
// blend (additive without blend is an opaque draw and shares the opaque pipeline). The ORDER of transparent draws is the
// caller's (the engine draws the Transparent queue back to front); the renderer draws in submission order.
//
// TEXT (G3b). The text shader is the SDF shader of OpenGL's: antialiased with fwidth; the effects (outline, shadow, softness) are
// material values snapshotted into the ring at every DrawMesh, so a multi-pass effect (one material and mesh, the uniforms
// changed between DrawMesh calls) draws each pass with its own values. The atlas is the material's base texture; a text material
// whose atlas is absent or destroyed gets the renderer's 1x1 (0,0,0,0) texture, so nothing is drawn.

namespace Renderer::WebGpu
{
    using Common::IMaterial;
    using Common::IMesh;
    using Common::IShader;
    using Common::ITexture;
    using Common::RenderState;
    using Common::TextureFilter;
    using Common::TextureOptions;
    using Common::TextureWrap;
    using ShaderInterface::BuiltInShader;
    using ShaderInterface::kFirstSamplerBinding;
    using ShaderInterface::kLightingBlockSize;
    using ShaderInterface::kMaterialBlockRange;
    using ShaderInterface::kPerDrawBindingSize;
    using ShaderInterface::kPerDrawBlockSize;
    using ShaderInterface::kSamplerCount;

    namespace
    {
        std::size_t ShaderIndex(const BuiltInShader kind)
        {
            return static_cast<std::size_t>(kind);
        }

        /// Which vertex attribute set the shader reads: unlit and text read (position, texture coordinates, colour), lit all five
        std::uint8_t VertexLayoutId(const BuiltInShader kind)
        {
            return kind == BuiltInShader::Lit ? 1 : 0;
        }

        WGPUVertexFormat AttributeFormat(const std::uint32_t components)
        {
            switch (components)
            {
            case 2:
                return WGPUVertexFormat_Float32x2;
            case 3:
                return WGPUVertexFormat_Float32x3;
            default:
                return WGPUVertexFormat_Float32x4;
            }
        }

        void OnScopePopped(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void* userdata1, void*)
        {
            std::unique_ptr<std::shared_ptr<ScopeResult>> holder(static_cast<std::shared_ptr<ScopeResult>*>(userdata1));
            ScopeResult& result = **holder;
            std::lock_guard lock(result.mutex);
            result.failed = status == WGPUPopErrorScopeStatus_Success && type != WGPUErrorType_NoError;
            result.message = ToString(message);
            result.done = true;
        }

        /// Opens a validation error scope on the device, runs `work` (which makes the device objects whose errors are wanted)
        /// and pops the scope, so an error in `work` is captured here instead of counting as an uncaptured one. Natively the
        /// pop is waited for (a few seconds at most): the result is final on return. In the browser it cannot be (no blocking
        /// wait), so the result arrives from the event loop and `done` is false on return. `pending` is the block the callback
        /// writes: the caller may keep it to look at the outcome later
        template <typename Work>
        std::shared_ptr<ScopeResult> RunInValidationScope(WebGpuRenderer::State& state, const Work& work)
        {
            auto result = std::make_shared<ScopeResult>();
            wgpuDevicePushErrorScope(state.device.Get(), WGPUErrorFilter_Validation);
            work();
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.callback = OnScopePopped;
            callback.userdata1 = new std::shared_ptr<ScopeResult>(result);
#ifdef __EMSCRIPTEN__
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            wgpuDevicePopErrorScope(state.device.Get(), callback);
#else
            callback.mode = WGPUCallbackMode_WaitAnyOnly;
            const WGPUFuture future = wgpuDevicePopErrorScope(state.device.Get(), callback);
            WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
            info.future = future;
            constexpr std::uint64_t timeoutNanoseconds = 5ull * 1000 * 1000 * 1000;
            wgpuInstanceWaitAny(state.instance.Get(), 1, &info, timeoutNanoseconds);
#endif
            return result;
        }

        /// Writes a row-major 4x4 matrix as its columns: the transpose, which is how a mat4x4<f32> in a uniform block is read
        void WriteTransposed(std::uint8_t* destination, const float* rowMajor)
        {
            float columns[16];
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    columns[column * 4 + row] = rowMajor[row * 4 + column];
                }
            }
            std::memcpy(destination, columns, sizeof(columns));
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The draw resources: shader module, layouts, default textures
    // -----------------------------------------------------------------------------------------------------------------

    bool WebGpuRenderer::CreateDrawResources()
    {
        State& state = *_state;

        // The three shaders: one module each, holding vs_main and fs_main. A WGSL error is reported by the device (counted by
        // the error counters, which fail Initialize), not by a null module
        {
            struct Source
            {
                BuiltInShader kind;
                std::string_view code;
                const char* label;
            };
            const std::array<Source, kShaderCount> sources{{
                {BuiltInShader::Unlit, Wgsl::kUnlitWgsl, "N2Engine unlit shader"},
                {BuiltInShader::Lit, Wgsl::kLitWgsl, "N2Engine lit shader"},
                {BuiltInShader::Text, Wgsl::kTextWgsl, "N2Engine text shader"},
            }};
            for (const Source& entry : sources)
            {
                auto shader = std::make_unique<WebGpuShader>(entry.kind);
                WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
                source.code = WGPUStringView{entry.code.data(), entry.code.size()};
                WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
                descriptor.label = ToView(entry.label);
                descriptor.nextInChain = &source.chain;
                shader->module.Reset(wgpuDeviceCreateShaderModule(state.device.Get(), &descriptor));
                if (!shader->module)
                {
                    return Fail(InitStatus::Failed, std::string("could not create the ") + entry.label + " module");
                }
                state.shaders[ShaderIndex(entry.kind)] = std::move(shader);
            }
        }

        // The vertex stage of every user shader: one module (vs_main), the contract's five varyings always written
        {
            WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
            source.code = WGPUStringView{Wgsl::kUserWgsl.data(), Wgsl::kUserWgsl.size()};
            WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine user shader vertex stage");
            descriptor.nextInChain = &source.chain;
            state.userVertexModule.Reset(wgpuDeviceCreateShaderModule(state.device.Get(), &descriptor));
            if (!state.userVertexModule)
            {
                return Fail(InitStatus::Failed, "could not create the user shader vertex module");
            }
        }

        // Group 0: the per-draw block, then the lighting block
        {
            std::array<WGPUBindGroupLayoutEntry, 2> entries;
            for (std::uint32_t i = 0; i < entries.size(); ++i)
            {
                entries[i] = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
                entries[i].binding = i;
                entries[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
                entries[i].buffer.type = WGPUBufferBindingType_Uniform;
                entries[i].buffer.hasDynamicOffset = WGPU_TRUE;
                entries[i].buffer.minBindingSize = 0;
            }
            WGPUBindGroupLayoutDescriptor descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine frame group");
            descriptor.entryCount = entries.size();
            descriptor.entries = entries.data();
            state.frameLayout.Reset(wgpuDeviceCreateBindGroupLayout(state.device.Get(), &descriptor));
        }

        // Group 1: the material block, then the five textures, then their five samplers
        {
            std::array<WGPUBindGroupLayoutEntry, 1 + 2 * kSamplerCount> entries;
            for (WGPUBindGroupLayoutEntry& entry : entries)
            {
                entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            }
            entries[0].binding = 0;
            entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            entries[0].buffer.type = WGPUBufferBindingType_Uniform;
            entries[0].buffer.hasDynamicOffset = WGPU_TRUE;
            entries[0].buffer.minBindingSize = 0;
            for (std::uint32_t i = 0; i < kSamplerCount; ++i)
            {
                WGPUBindGroupLayoutEntry& texture = entries[1 + i];
                texture.binding = kFirstSamplerBinding + i;
                texture.visibility = WGPUShaderStage_Fragment;
                texture.texture.sampleType = WGPUTextureSampleType_Float;
                texture.texture.viewDimension = WGPUTextureViewDimension_2D;
                texture.texture.multisampled = WGPU_FALSE;

                WGPUBindGroupLayoutEntry& sampler = entries[1 + kSamplerCount + i];
                sampler.binding = kFirstSamplerBinding + kSamplerCount + i;
                sampler.visibility = WGPUShaderStage_Fragment;
                sampler.sampler.type = WGPUSamplerBindingType_Filtering;
            }
            WGPUBindGroupLayoutDescriptor descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine material group");
            descriptor.entryCount = entries.size();
            descriptor.entries = entries.data();
            state.materialLayout.Reset(wgpuDeviceCreateBindGroupLayout(state.device.Get(), &descriptor));
        }
        if (!state.frameLayout || !state.materialLayout)
        {
            return Fail(InitStatus::Failed, "could not create the bind group layouts");
        }

        {
            const WGPUBindGroupLayout layouts[2] = {state.frameLayout.Get(), state.materialLayout.Get()};
            WGPUPipelineLayoutDescriptor descriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine pipeline layout");
            descriptor.bindGroupLayoutCount = 2;
            descriptor.bindGroupLayouts = layouts;
            state.pipelineLayout.Reset(wgpuDeviceCreatePipelineLayout(state.device.Get(), &descriptor));
            if (!state.pipelineLayout)
            {
                return Fail(InitStatus::Failed, "could not create the pipeline layout");
            }
        }

        // 1x1 textures for the slots a material leaves empty: a white base, a black emissive, a white occlusion, a flat
        // normal and a white metallic-roughness (the same defaults as the Vulkan backend)
        const std::array<std::array<std::uint8_t, 4>, kSamplerCount> colours{{
            {255, 255, 255, 255},
            {0, 0, 0, 255},
            {255, 255, 255, 255},
            {128, 128, 255, 255},
            {255, 255, 255, 255},
        }};
        TextureOptions options;
        options.filter = TextureFilter::Nearest;
        options.wrap = TextureWrap::ClampToEdge;
        options.mipmaps = false;
        for (std::size_t i = 0; i < kSamplerCount; ++i)
        {
            std::unique_ptr<WebGpuTexture> texture = MakeTexture(state, colours[i].data(), 1, 1, 4, options);
            if (!texture)
            {
                return Fail(InitStatus::Failed, "could not create a default texture");
            }
            texture->id = state.nextTextureId++;
            state.defaults[i] = std::move(texture);
        }
        {
            const std::array<std::uint8_t, 4> transparent{0, 0, 0, 0};
            std::unique_ptr<WebGpuTexture> texture = MakeTexture(state, transparent.data(), 1, 1, 4, options);
            if (!texture)
            {
                return Fail(InitStatus::Failed, "could not create the text default texture");
            }
            texture->id = state.nextTextureId++;
            state.textBaseDefault = std::move(texture);
        }
        return true;
    }

    void WebGpuRenderer::DestroyAllResources()
    {
        State& state = *_state;
        // Bind groups, ring buffers and pipelines first (they name the rest), then what they name. Releasing is all that is
        // needed: the device is destroyed right after this
        state.materialGroups.clear();
        state.chunks.clear();
        state.activeChunk = 0;
        state.pipelines.clear();
        state.failedPipelines.clear();
        state.userPipelineUse.clear();
        state.pipelineTick = 0;
        state.pipelineCreateAttempts = 0;
        state.pipelineLayout.Reset();
        state.materialLayout.Reset();
        state.frameLayout.Reset();
        for (auto& shader : state.shaders)
        {
            shader.reset();
        }
        state.liveUserShaders.clear();
        state.userShaders.clear();
        state.freeUserIds.clear();
        state.userVertexModule.Reset();
        state.materials.clear();
        for (auto& texture : state.defaults)
        {
            texture.reset();
        }
        state.textBaseDefault.reset();
        state.textures.clear();
        state.meshes.clear();
        state.samplers.clear();
        {
            std::lock_guard idLock(state.textureIdMutex);
            state.textureIds.clear();
        }
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(state.view, identity, sizeof(identity));
        std::memcpy(state.projection, identity, sizeof(identity));
        state.lighting = Gpu::BuildLightingBlock(Common::SceneLightingData{}, N2Engine::Math::Vector3(0.0f, 0.0f, 0.0f));
        ++state.lightingVersion;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The uniform ring
    // -----------------------------------------------------------------------------------------------------------------

    void WebGpuRenderer::BeginDrawFrame()
    {
        State& state = *_state;
        for (RingChunk& chunk : state.chunks)
        {
            chunk.used = 0;
            chunk.lightingVersion = 0; // the lighting copy of an earlier frame is not this frame's
            chunk.lightingOffset = 0;
        }
        state.activeChunk = 0;
    }

    void WebGpuRenderer::EndDrawFrame()
    {
        State& state = *_state;
        const bool canWrite = state.queue && !state.lost.load();
        for (RingChunk& chunk : state.chunks)
        {
            if (canWrite && chunk.used > 0)
            {
                // One write per chunk: the used part, which is a whole number of aligned slots (and so a multiple of 4)
                wgpuQueueWriteBuffer(state.queue.Get(), chunk.buffer.Get(), 0, chunk.staging.data(),
                                     static_cast<std::size_t>(AlignUp64(chunk.used, 4)));
            }
            chunk.used = 0;
            chunk.lightingVersion = 0;
            chunk.lightingOffset = 0;
        }
        state.activeChunk = 0;
    }

    bool WebGpuRenderer::AddRingChunk(const std::uint64_t minimumSize)
    {
        State& state = *_state;
        // The size of a new chunk, a multiple of the alignment; dynamic offsets are 32-bit, so a chunk stays under 4 GiB. Never
        // smaller than the lighting block: group 0's binding 1 is bound at offset 0 by a draw that does not read it
        const std::uint64_t size =
            AlignUp64(std::max({state.ringChunkBytes, minimumSize, static_cast<std::uint64_t>(kLightingBlockSize)}),
                      state.uniformAlignment);
        if (size > state.maxBufferSize || size > std::numeric_limits<std::uint32_t>::max())
        {
            LogOnce("the uniform ring cannot grow: a chunk of " + std::to_string(size) +
                    " bytes is over the device's buffer limit; draws are skipped");
            return false;
        }
        WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine uniform ring");
        descriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        descriptor.size = size;
        RingChunk chunk;
        chunk.buffer.Reset(wgpuDeviceCreateBuffer(state.device.Get(), &descriptor));
        if (!chunk.buffer)
        {
            LogOnce("could not make a uniform ring buffer; draws are skipped");
            return false;
        }
        chunk.staging.assign(static_cast<std::size_t>(size), 0);
        chunk.size = size;
        chunk.used = 0;
        state.chunks.push_back(std::move(chunk));
        state.activeChunk = state.chunks.size() - 1;
        return true;
    }

    bool WebGpuRenderer::AllocateRing(const std::uint64_t bytes, std::size_t& chunkIndex, std::uint64_t& offset)
    {
        State& state = *_state;
        // The bytes are a whole number of aligned slots, so every offset handed out is a multiple of the alignment
        while (state.activeChunk < state.chunks.size())
        {
            RingChunk& chunk = state.chunks[state.activeChunk];
            if (chunk.used + bytes <= chunk.size)
            {
                chunkIndex = state.activeChunk;
                offset = chunk.used;
                chunk.used += bytes;
                return true;
            }
            ++state.activeChunk; // full for this draw: the next chunk (every chunk is empty at the start of a frame)
        }
        if (!AddRingChunk(bytes))
        {
            return false;
        }
        RingChunk& chunk = state.chunks[state.activeChunk];
        chunkIndex = state.activeChunk;
        offset = chunk.used;
        chunk.used += bytes;
        return true;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Pipelines
    // -----------------------------------------------------------------------------------------------------------------

    WGPURenderPipeline WebGpuRenderer::GetPipelineLocked(const WebGpuShader* shader, const RenderState& renderState)
    {
        State& state = *_state;
        if (shader == nullptr || !shader->IsValid() || !state.pipelineLayout)
        {
            return nullptr;
        }
        const bool user = shader->IsUserShader();
        if (user)
        {
            // A shader the device rejected (known at once natively; in the browser once its error scope has been delivered)
            // is skipped: its pipelines would only raise more errors
            if (shader->check)
            {
                std::lock_guard checkLock(shader->check->mutex);
                if (shader->check->failed)
                {
                    LogOnce("user shader '" + shader->label + "' was rejected by the device: " + shader->check->message);
                    return nullptr;
                }
            }
            if (!state.userVertexModule)
            {
                return nullptr;
            }
        }
        const BuiltInShader kind = shader->kind;

        PipelineKey key;
        key.shader = shader->id;
        // A user shader reads the lit shader's attribute set (all five: the vertex stage writes the five varyings)
        key.vertexLayout = user ? 1 : VertexLayoutId(kind);
        key.depthTest = renderState.depthTest;
        // Nothing writes depth while the test is off (as OpenGL): a draw with the test off and the write on shares the pipeline
        // of the draw with both off
        key.depthWrite = renderState.depthTest && renderState.depthWrite;
        key.cull = static_cast<std::uint8_t>(renderState.cull);
        key.blend = renderState.blend;
        key.additive = renderState.blend && renderState.additive; // additive without blend is an opaque draw, as in GL
        key.variant = (renderState.blend && !renderState.additive) ? static_cast<std::uint8_t>(renderState.variant) : 0;
        const std::uint32_t packed = key.Pack();
        if (const auto it = state.pipelines.find(packed); it != state.pipelines.end())
        {
            if (user)
            {
                state.userPipelineUse[packed] = ++state.pipelineTick;
            }
            return it->second.Get();
        }
        if (state.failedPipelines.count(packed) != 0)
        {
            return nullptr; // this key failed before (logged once): skipped without retrying
        }
        if (user && state.userPipelineUse.size() >= state.maxUserPipelines)
        {
            // Past the limit (a hostile pack could grow shaders x states without bound): release the least recently used
            // user pipelines down to three quarters of it. A recorded command keeps what it uses alive, so nothing waits
            const std::size_t keep = std::max<std::size_t>(1, state.maxUserPipelines * 3 / 4);
            LogOnce("user shader pipeline limit (" + std::to_string(state.maxUserPipelines) +
                    ") reached: the least recently used user pipelines are released");
            std::vector<std::pair<std::uint64_t, std::uint32_t>> byUse;
            byUse.reserve(state.userPipelineUse.size());
            for (const auto& [pipelineKey, tick] : state.userPipelineUse)
            {
                byUse.emplace_back(tick, pipelineKey);
            }
            std::sort(byUse.begin(), byUse.end());
            for (std::size_t i = 0; i < byUse.size() && state.userPipelineUse.size() > keep; ++i)
            {
                state.pipelines.erase(byUse[i].second);
                state.userPipelineUse.erase(byUse[i].second);
            }
        }
        // One interleaved vertex buffer of Common::Vertex (64 bytes); exactly the attributes the shader reads
        std::array<WGPUVertexAttribute, 5> attributes;
        std::uint32_t attributeCount = 0;
        for (const ShaderInterface::Attribute& attribute : ShaderInterface::Attributes(user ? BuiltInShader::Lit : kind))
        {
            WGPUVertexAttribute description = WGPU_VERTEX_ATTRIBUTE_INIT;
            description.format = AttributeFormat(attribute.components);
            description.offset = attribute.offset;
            description.shaderLocation = attribute.location;
            attributes[attributeCount++] = description;
        }
        WGPUVertexBufferLayout vertexLayout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        vertexLayout.stepMode = WGPUVertexStepMode_Vertex;
        vertexLayout.arrayStride = sizeof(Common::Vertex);
        vertexLayout.attributeCount = attributeCount;
        vertexLayout.attributes = attributes.data();

        // GL: depth function LESS; the test off is ALWAYS
        WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
        depthStencil.format = GetDepthFormat();
        depthStencil.depthWriteEnabled = key.depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        depthStencil.depthCompare = key.depthTest ? WGPUCompareFunction_Less : WGPUCompareFunction_Always;

        // Blending exactly as OpenGL's ApplyRenderState (and Vulkan's pipeline): SRC_ALPHA with ONE_MINUS_SRC_ALPHA (additive:
        // ONE), the same factors for colour and alpha, equation ADD
        WGPUBlendState blendState = WGPU_BLEND_STATE_INIT;
        const Renderer::Common::BlendFunc func =
            Renderer::Common::BlendFuncFor(key.additive, static_cast<Renderer::Common::BlendVariant>(key.variant));
        const auto factor = [](const Renderer::Common::BlendFactor f) {
            switch (f)
            {
            case Renderer::Common::BlendFactor::Zero:
                return WGPUBlendFactor_Zero;
            case Renderer::Common::BlendFactor::One:
                return WGPUBlendFactor_One;
            case Renderer::Common::BlendFactor::SrcAlpha:
                return WGPUBlendFactor_SrcAlpha;
            case Renderer::Common::BlendFactor::OneMinusSrcAlpha:
                return WGPUBlendFactor_OneMinusSrcAlpha;
            case Renderer::Common::BlendFactor::DstColor:
                return WGPUBlendFactor_Dst;
            }
            return WGPUBlendFactor_One;
        };
        blendState.color.operation = WGPUBlendOperation_Add;
        blendState.color.srcFactor = factor(func.srcColor);
        blendState.color.dstFactor = factor(func.dstColor);
        blendState.alpha.operation = WGPUBlendOperation_Add;
        blendState.alpha.srcFactor = factor(func.srcAlpha);
        blendState.alpha.dstFactor = factor(func.dstAlpha);

        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = GetColorFormat();
        target.blend = key.blend ? &blendState : nullptr; // null: opaque
        target.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = shader->module.Get();
        fragment.entryPoint = ToView(user ? "main" : "fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine pipeline");
        descriptor.layout = state.pipelineLayout.Get();
        // A user shader's custom vertex stage (stage 9d) is its own module with the entry point main; otherwise the shared one (vs_main)
        const bool ownVertex = user && shader->vertexModule;
        descriptor.vertex.module = ownVertex ? shader->vertexModule.Get() : (user ? state.userVertexModule.Get() : shader->module.Get());
        descriptor.vertex.entryPoint = ToView(ownVertex ? "main" : "vs_main");
        descriptor.vertex.bufferCount = 1;
        descriptor.vertex.buffers = &vertexLayout;
        descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        // GL's front faces are counter-clockwise as seen on the screen, which is WebGPU's CCW too
        descriptor.primitive.frontFace = WGPUFrontFace_CCW;
        descriptor.primitive.cullMode = renderState.cull == Common::CullMode::Back    ? WGPUCullMode_Back
            : renderState.cull == Common::CullMode::Front                             ? WGPUCullMode_Front
                                                                                      : WGPUCullMode_None;
        descriptor.depthStencil = &depthStencil;
        descriptor.fragment = &fragment;

        ++state.pipelineCreateAttempts;
        RenderPipelineRef pipeline;
        std::string rejection;
        if (!state.failPipelineCreationForTest)
        {
            if (user)
            {
                // A user shader's WGSL is untrusted: a module the device accepted may still not fit the pipeline (a binding the
                // layout does not have, an input the vertex stage does not write). The error is captured here, not counted as an
                // uncaptured one, and the key becomes a remembered failure
                const std::shared_ptr<ScopeResult> scope = RunInValidationScope(
                    state, [&] { pipeline.Reset(wgpuDeviceCreateRenderPipeline(state.device.Get(), &descriptor)); });
                std::lock_guard scopeLock(scope->mutex);
                if (scope->done && scope->failed)
                {
                    rejection = scope->message;
                    pipeline.Reset();
                }
            }
            else
            {
                pipeline.Reset(wgpuDeviceCreateRenderPipeline(state.device.Get(), &descriptor));
            }
        }
        if (!pipeline)
        {
            // Remember the failure (a negative cache entry) so every later draw with this key skips quietly; one line per key
            state.failedPipelines.insert(packed);
            LogOnce("could not create the render pipeline for key " + std::to_string(packed) +
                    ": draws with that state are skipped" + (rejection.empty() ? std::string() : ": " + rejection));
            return nullptr;
        }
        const WGPURenderPipeline raw = pipeline.Get();
        state.pipelines.emplace(packed, std::move(pipeline));
        if (user)
        {
            state.userPipelineUse[packed] = ++state.pipelineTick;
        }
        return raw;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Shaders and materials
    // -----------------------------------------------------------------------------------------------------------------

    Common::IShader* WebGpuRenderer::CreateShaderProgram(const char* vertexSource, const char* fragmentSource)
    {
        static_cast<void>(vertexSource);
        static_cast<void>(fragmentSource);
        // Custom shaders are not supported in v1 on any GPU backend: the three built-ins are the shaders
        LogOnce("CreateShaderProgram: custom shaders are not supported (the built-in shaders are the shaders); returning null");
        return nullptr;
    }

    void WebGpuRenderer::UseShaderProgram(Common::IShader* shader)
    {
        static_cast<void>(shader); // a draw takes its shader from its material
    }

    bool WebGpuRenderer::DestroyShaderProgram(Common::IShader* shader)
    {
        if (shader == nullptr)
        {
            return false;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        // The built-in shaders belong to the renderer: only a live user shader is destroyed (a foreign pointer is only compared)
        const auto found = state.liveUserShaders.find(shader);
        if (found == state.liveUserShaders.end())
        {
            return false;
        }
        const std::uint16_t id = found->second->id;
        // Its pipelines go now (a command already recorded keeps what it uses alive), and so does what was remembered of keys
        // that failed for it: the id is recycled, and a failure says nothing about the next shader
        for (auto it = state.pipelines.begin(); it != state.pipelines.end();)
        {
            if (PipelineKey::ShaderOf(it->first) == id)
            {
                state.userPipelineUse.erase(it->first);
                it = state.pipelines.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = state.failedPipelines.begin(); it != state.failedPipelines.end();)
        {
            it = PipelineKey::ShaderOf(*it) == id ? state.failedPipelines.erase(it) : std::next(it);
        }
        state.liveUserShaders.erase(found);
        state.userShaders[id - kShaderCount].reset();
        state.freeUserIds.push_back(id);
        return true;
    }

    bool WebGpuRenderer::IsValidShader(Common::IShader* shader) const
    {
        if (shader == nullptr)
        {
            return false;
        }
        std::lock_guard lock(_mutex);
        for (const auto& builtIn : _state->shaders)
        {
            if (builtIn != nullptr && builtIn->IsValid() && shader == builtIn.get())
            {
                return true;
            }
        }
        return _state->liveUserShaders.count(shader) != 0;
    }

    WebGpuShader* WebGpuRenderer::FindUserShaderLocked(const Common::IShader* shader) const
    {
        const auto found = _state->liveUserShaders.find(shader);
        return found != _state->liveUserShaders.end() ? found->second : nullptr;
    }

    Common::IShader* WebGpuRenderer::CreateUserShader(const Common::UserShaderBlob& blob)
    {
        const std::string label = blob.sourcePath.empty() ? blob.name : blob.sourcePath;
        // Everything a hostile blob can get wrong is checked before the device sees a byte of it
        if (const std::string problem = Common::ValidateUserShaderBlob(blob, Common::UserShaderTarget::WebGpu); !problem.empty())
        {
            LogOnce("CreateUserShader: '" + label + "' rejected: " + problem);
            return nullptr;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load() || !state.userVertexModule)
        {
            return nullptr;
        }
        std::size_t used = 0;
        for (const auto& slot : state.userShaders)
        {
            used += slot ? 1 : 0;
        }
        if (used >= Common::kMaxUserShaders)
        {
            LogOnce("CreateUserShader: '" + label + "' refused: " + std::to_string(Common::kMaxUserShaders) +
                    " user shaders are already loaded");
            return nullptr;
        }
        std::shared_ptr<const Gpu::MaterialLayout> layout = Gpu::MaterialLayout::FromUserShader(blob);
        if (!layout)
        {
            return nullptr; // unreachable after the validation above
        }

        // The device validates the WGSL; the error is captured by a scope (not counted as an uncaptured one)
        ShaderModuleRef module;
        ShaderModuleRef vertexModule; // a custom vertex stage, when the blob has one (stage 9d)
        const std::shared_ptr<ScopeResult> scope = RunInValidationScope(
            state,
            [&]
            {
                WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
                source.code = WGPUStringView{blob.fragmentWgsl.data(), blob.fragmentWgsl.size()};
                WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
                descriptor.label = ToView("N2Engine user shader");
                descriptor.nextInChain = &source.chain;
                module.Reset(wgpuDeviceCreateShaderModule(state.device.Get(), &descriptor));
                if (!blob.vertexWgsl.empty())
                {
                    WGPUShaderSourceWGSL vertexSource = WGPU_SHADER_SOURCE_WGSL_INIT;
                    vertexSource.code = WGPUStringView{blob.vertexWgsl.data(), blob.vertexWgsl.size()};
                    WGPUShaderModuleDescriptor vertexDescriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
                    vertexDescriptor.label = ToView("N2Engine user shader vertex");
                    vertexDescriptor.nextInChain = &vertexSource.chain;
                    vertexModule.Reset(wgpuDeviceCreateShaderModule(state.device.Get(), &vertexDescriptor));
                }
            });
        {
            // Natively the verdict is in. In the browser it is not yet: the shader is kept, and its draws are skipped if the
            // verdict (kept in shader->check) turns out to be a rejection
            std::lock_guard scopeLock(scope->mutex);
            if (scope->done && scope->failed)
            {
                LogOnce("CreateUserShader: '" + label + "': the device rejected the WGSL: " + scope->message);
                return nullptr;
            }
        }
        if (!module || (!blob.vertexWgsl.empty() && !vertexModule))
        {
            LogOnce("CreateUserShader: '" + label + "': could not create the shader module");
            return nullptr;
        }

        std::size_t slotIndex = state.userShaders.size();
        if (!state.freeUserIds.empty())
        {
            slotIndex = state.freeUserIds.back() - kShaderCount;
            state.freeUserIds.pop_back();
        }
        else
        {
            state.userShaders.emplace_back();
        }
        const auto id = static_cast<std::uint16_t>(kShaderCount + slotIndex);
        auto shader = std::make_unique<WebGpuShader>(id, std::move(layout), blob.usesLighting, label);
        shader->module = std::move(module);
        shader->vertexModule = std::move(vertexModule);
        shader->check = scope;
        WebGpuShader* raw = shader.get();
        state.userShaders[slotIndex] = std::move(shader);
        state.liveUserShaders.emplace(raw, raw);
        return raw;
    }

    Common::IMaterial* WebGpuRenderer::CreateUserMaterial(Common::IShader* shader, Common::ITexture* texture)
    {
        if (shader == nullptr)
        {
            return nullptr;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load())
        {
            return nullptr;
        }
        const WebGpuShader* user = FindUserShaderLocked(shader);
        if (user == nullptr)
        {
            LogOnce("CreateUserMaterial: the shader is not a live user shader of this renderer; returning null");
            return nullptr;
        }
        State* const statePointer = _state.get(); // the materials die with it, so it outlives them
        auto material = std::make_unique<Gpu::BlockMaterial>(
            user->layout, shader, texture, [this](const std::string& message) { LogOnce(message); },
            [statePointer](const ITexture* candidate) -> std::uint64_t
            {
                std::lock_guard idLock(statePointer->textureIdMutex);
                const auto found = statePointer->textureIds.find(candidate);
                return found != statePointer->textureIds.end() ? found->second : 0;
            });
        IMaterial* raw = material.get();
        state.materials.emplace(raw, std::move(material));
        return raw;
    }

    Common::IShader* WebGpuRenderer::GetStandardShader(const BuiltInShader kind) const
    {
        std::lock_guard lock(_mutex);
        const WebGpuShader* shader = _state->shaders[ShaderIndex(kind)].get();
        if (!_initialized || shader == nullptr || !shader->IsValid())
        {
            return nullptr;
        }
        return const_cast<WebGpuShader*>(shader);
    }

    Common::IShader* WebGpuRenderer::GetStandardUnlitShader() const
    {
        return GetStandardShader(BuiltInShader::Unlit);
    }

    Common::IShader* WebGpuRenderer::GetStandardLitShader() const
    {
        return GetStandardShader(BuiltInShader::Lit);
    }

    Common::IShader* WebGpuRenderer::GetStandardTextShader() const
    {
        return GetStandardShader(BuiltInShader::Text);
    }

    Common::IMaterial* WebGpuRenderer::CreateMaterial(Common::IShader* shader)
    {
        return CreateMaterial(shader, nullptr);
    }

    Common::IMaterial* WebGpuRenderer::CreateMaterial(Common::IShader* shader, Common::ITexture* texture)
    {
        if (shader == nullptr)
        {
            return nullptr;
        }
        std::lock_guard lock(_mutex);
        State& state = *_state;
        if (!_initialized || state.lost.load())
        {
            return nullptr;
        }
        // A foreign shader is only compared with the built-ins, never dereferenced
        const WebGpuShader* chosen = nullptr;
        for (const auto& builtIn : state.shaders)
        {
            if (builtIn != nullptr && builtIn->IsValid() && shader == builtIn.get())
            {
                chosen = builtIn.get();
            }
        }
        if (chosen == nullptr)
        {
            LogOnce("CreateMaterial: only materials of this renderer's built-in shaders (unlit, lit, text) exist; returning null");
            return nullptr;
        }

        State* const statePointer = _state.get(); // the materials die with it, so it outlives them
        auto material = std::make_unique<Gpu::BlockMaterial>(
            chosen->kind, shader, texture, [this](const std::string& message) { LogOnce(message); },
            [statePointer](const ITexture* candidate) -> std::uint64_t
            {
                std::lock_guard idLock(statePointer->textureIdMutex);
                const auto found = statePointer->textureIds.find(candidate);
                return found != statePointer->textureIds.end() ? found->second : 0;
            });
        IMaterial* raw = material.get();
        state.materials.emplace(raw, std::move(material));
        return raw;
    }

    void WebGpuRenderer::DestroyMaterial(Common::IMaterial* material)
    {
        if (material == nullptr)
        {
            return;
        }
        std::lock_guard lock(_mutex);
        // A draw copied what it needs into the ring when it was recorded, so a material can go at once, even in a frame that
        // used it. Foreign pointers are not keys and are ignored
        _state->materials.erase(material);
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Drawing
    // -----------------------------------------------------------------------------------------------------------------

    void WebGpuRenderer::SetViewProjection(const float* view, const float* projection)
    {
        std::lock_guard lock(_mutex);
        if (view != nullptr)
        {
            std::memcpy(_state->view, view, sizeof(_state->view));
        }
        if (projection != nullptr)
        {
            std::memcpy(_state->projection, projection, sizeof(_state->projection));
        }
    }

    void WebGpuRenderer::UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                             const N2Engine::Math::Vector3& cameraPosition)
    {
        // Only the CPU copy changes; the first lit draw after this copies it into the ring (see the header comment)
        const Gpu::LightingBlock block = Gpu::BuildLightingBlock(lighting, cameraPosition);
        std::lock_guard lock(_mutex);
        _state->lighting = block;
        ++_state->lightingVersion;
    }

    void WebGpuRenderer::SetWireframe(bool enabled)
    {
        if (enabled)
        {
            // Not in core WebGPU (no polygon mode): ignored in v1, design decision W9
            LogOnce("SetWireframe: wireframe is not supported by the WebGPU backend (no polygon mode in core WebGPU): ignored");
        }
    }

    void WebGpuRenderer::DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                                  const Common::RenderState& state)
    {
        std::lock_guard lock(_mutex);
        DrawLocked(mesh, modelMatrix, material, state, 0, 0, true);
    }

    void WebGpuRenderer::DrawMesh(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                                  const Common::RenderState& state, const Common::IndexRange& range)
    {
        std::lock_guard lock(_mutex);
        DrawLocked(mesh, modelMatrix, material, state, range.first, range.count, false);
    }

    void WebGpuRenderer::DrawObjects(const std::vector<Common::RenderObject>& objects)
    {
        std::lock_guard lock(_mutex);
        for (const Common::RenderObject& object : objects)
        {
            DrawLocked(object.mesh, object.transform.model, object.material, object.state, 0, 0, true);
        }
    }

    void WebGpuRenderer::DrawLocked(Common::IMesh* mesh, const float* modelMatrix, Common::IMaterial* material,
                                    const Common::RenderState& renderState, std::uint32_t first, std::uint32_t count,
                                    const bool wholeMesh)
    {
        State& state = *_state;
        if (!_initialized || state.lost.load() || !_frameOpen || !state.pass || mesh == nullptr || modelMatrix == nullptr ||
            material == nullptr)
        {
            return;
        }

        // Foreign (or destroyed) pointers are only looked up in the registries, never dereferenced
        const auto meshIt = state.meshes.find(mesh);
        if (meshIt == state.meshes.end())
        {
            return;
        }
        // Read under the lock: UpdateMesh may swap the buffers. A mesh with vertices and no indices has no index buffer and
        // draws nothing (as OpenGL and Vulkan)
        WebGpuMesh& webMesh = *meshIt->second;
        if (!webMesh.vertexBuffer || !webMesh.indexBuffer || webMesh.indexCount == 0)
        {
            return;
        }
        if (wholeMesh)
        {
            first = 0;
            count = webMesh.indexCount;
        }
        else if (count == 0 || static_cast<std::uint64_t>(first) + count > webMesh.indexCount)
        {
            return; // GL semantics: a range that is not inside the index buffer draws nothing
        }

        const auto materialIt = state.materials.find(material);
        if (materialIt == state.materials.end())
        {
            return;
        }
        const Gpu::BlockMaterial& blockMaterial = *materialIt->second;

        // A user material draws with its shader if that is still alive (a destroyed shader's materials draw nothing)
        const WebGpuShader* drawShader = nullptr;
        if (blockMaterial.IsUserShader())
        {
            drawShader = FindUserShaderLocked(blockMaterial.GetShader());
        }
        else
        {
            drawShader = state.shaders[ShaderIndex(blockMaterial.kind)].get();
        }
        if (drawShader == nullptr)
        {
            return;
        }
        const WGPURenderPipeline pipeline = GetPipelineLocked(drawShader, renderState);
        if (pipeline == nullptr)
        {
            return; // the pipeline could not be made (logged)
        }

        // The textures: a texture the renderer no longer has (destroyed since it was set) is replaced by the 1x1 default
        const Gpu::BlockMaterial::Snapshot snapshot = blockMaterial.TakeSnapshot();
        std::array<const WebGpuTexture*, kSamplerCount> textures{};
        for (std::size_t i = 0; i < kSamplerCount; ++i)
        {
            const WebGpuTexture* texture = nullptr;
            if (snapshot.textures[i] != nullptr)
            {
                // Registered AND the same texture that was set: an id from the counter never repeats, so a texture made at the
                // address of a destroyed one is not taken for it (the slot gets the default, like any destroyed texture)
                if (const auto it = state.textures.find(snapshot.textures[i]);
                    it != state.textures.end() && snapshot.textureIds[i] != 0 && it->second->id == snapshot.textureIds[i])
                {
                    texture = it->second.get();
                }
            }
            if (texture == nullptr || !texture->view || texture->sampler == nullptr)
            {
                if (blockMaterial.IsUserShader())
                {
                    // An unset slot of a user shader reads the default its manifest declares
                    switch (blockMaterial.TextureDefault(i))
                    {
                    case Common::UserTextureDefault::Black: texture = state.defaults[1].get(); break;
                    case Common::UserTextureDefault::Normal: texture = state.defaults[3].get(); break;
                    case Common::UserTextureDefault::None: texture = state.textBaseDefault.get(); break;
                    case Common::UserTextureDefault::White: texture = state.defaults[0].get(); break;
                    }
                }
                else
                {
                    // The text shader's atlas is its base sampler, and an absent one must read sdf 0 (draw nothing), not white
                    texture = (i == 0 && blockMaterial.kind == BuiltInShader::Text) ? state.textBaseDefault.get() : state.defaults[i].get();
                }
            }
            if (texture == nullptr)
            {
                return;
            }
            textures[i] = texture;
        }

        // This draw's room in the ring: the per-draw block, then the material block, each at an aligned offset; and, for a lit
        // draw whose chunk does not hold the current lighting yet, the lighting block after them
        // A user shader reads the lighting when its manifest says so ("uses": ["lighting"]); the built-in lit shader always does
        const bool lit = drawShader->IsUserShader() ? drawShader->usesLighting : blockMaterial.kind == BuiltInShader::Lit;
        const std::uint64_t perDrawBytes = AlignUp64(kPerDrawBindingSize, state.uniformAlignment);
        // The material block's range: 128 for a built-in shader, the layout's (up to 256) for a user shader
        const std::uint64_t materialBytes = AlignUp64(snapshot.blockBytes, state.uniformAlignment);
        const std::uint64_t lightingBytes = AlignUp64(kLightingBlockSize, state.uniformAlignment);
        std::size_t chunkIndex = 0;
        std::uint64_t offset = 0;
        bool copyLighting = false;
        for (;;)
        {
            // Ask for the lighting too when the chunk the draw would start in does not hold this version of it. If that chunk
            // is full the allocation moves to the next, which holds none: then ask again with the lighting included
            copyLighting = lit &&
                !(state.activeChunk < state.chunks.size() && state.chunks[state.activeChunk].lightingVersion == state.lightingVersion);
            if (!AllocateRing(perDrawBytes + materialBytes + (copyLighting ? lightingBytes : 0), chunkIndex, offset))
            {
                return;
            }
            if (!lit || copyLighting || state.chunks[chunkIndex].lightingVersion == state.lightingVersion)
            {
                break;
            }
        }
        RingChunk& chunk = state.chunks[chunkIndex];

        // Row-major in, columns out: each matrix is transposed here, once (the WGSL reads the bytes as mat4x4<f32>)
        std::uint8_t* perDraw = chunk.staging.data() + offset;
        WriteTransposed(perDraw, modelMatrix);
        WriteTransposed(perDraw + 64, state.view);
        WriteTransposed(perDraw + 128, state.projection);
        // The prefab stage's dim and the screen tint, as this draw carries them (raised to SetStageDim's and
        // SetScreenTint's values), then the struct's padding: WGSL rounds PerDraw up to 224 bytes. @builtin(position)
        // counts rows down from the top, so the tint's circle is given that way
        ShaderInterface::WritePerDrawEffects(perDraw, WithFrameModifiers(renderState), _targetWidth, _targetHeight);
        std::memcpy(perDraw + perDrawBytes, snapshot.block.data(), snapshot.blockBytes);
        if (copyLighting)
        {
            // The first lit draw of this lighting version in this chunk: its copy goes after the material block
            chunk.lightingOffset = offset + perDrawBytes + materialBytes;
            chunk.lightingVersion = state.lightingVersion;
            std::memcpy(chunk.staging.data() + chunk.lightingOffset, state.lighting.data(), state.lighting.size());
        }

        // Group 0: this chunk's per-draw range and lighting range, each at a dynamic offset; made once per chunk
        if (!chunk.frameGroup)
        {
            std::array<WGPUBindGroupEntry, 2> entries;
            for (std::uint32_t i = 0; i < entries.size(); ++i)
            {
                entries[i] = WGPU_BIND_GROUP_ENTRY_INIT;
                entries[i].binding = i;
                entries[i].buffer = chunk.buffer.Get();
                entries[i].offset = 0;
            }
            entries[0].size = kPerDrawBindingSize;
            entries[1].size = kLightingBlockSize;
            WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine frame group");
            descriptor.layout = state.frameLayout.Get();
            descriptor.entryCount = entries.size();
            descriptor.entries = entries.data();
            chunk.frameGroup.Reset(wgpuDeviceCreateBindGroup(state.device.Get(), &descriptor));
            if (!chunk.frameGroup)
            {
                return;
            }
        }

        // Group 1: this chunk's material range at a dynamic offset and the five textures with their samplers. Immutable, so
        // draws with the same textures in the same chunk share one
        std::array<std::uint64_t, 2 + kSamplerCount> key{};
        key[0] = chunkIndex;
        key[1] = snapshot.blockBytes;
        for (std::size_t i = 0; i < kSamplerCount; ++i)
        {
            key[2 + i] = textures[i]->id;
        }
        WGPUBindGroup materialGroup = nullptr;
        if (const auto it = state.materialGroups.find(key); it != state.materialGroups.end())
        {
            materialGroup = it->second.Get();
        }
        else
        {
            std::array<WGPUBindGroupEntry, 1 + 2 * kSamplerCount> entries;
            for (WGPUBindGroupEntry& entry : entries)
            {
                entry = WGPU_BIND_GROUP_ENTRY_INIT;
            }
            entries[0].binding = 0;
            entries[0].buffer = chunk.buffer.Get();
            entries[0].offset = 0;
            entries[0].size = snapshot.blockBytes;
            for (std::uint32_t i = 0; i < kSamplerCount; ++i)
            {
                entries[1 + i].binding = kFirstSamplerBinding + i;
                entries[1 + i].textureView = textures[i]->view.Get();
                entries[1 + kSamplerCount + i].binding = kFirstSamplerBinding + kSamplerCount + i;
                entries[1 + kSamplerCount + i].sampler = textures[i]->sampler;
            }
            WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine material group");
            descriptor.layout = state.materialLayout.Get();
            descriptor.entryCount = entries.size();
            descriptor.entries = entries.data();
            BindGroupRef group(wgpuDeviceCreateBindGroup(state.device.Get(), &descriptor));
            if (!group)
            {
                return;
            }
            materialGroup = group.Get();
            state.materialGroups.emplace(key, std::move(group));
        }

        // Dynamic offsets, in binding order within each group: group 0 the per-draw block, then the lighting block (a draw that
        // does not read the lighting names the chunk's copy if it has one, else offset 0, which is in range and never read);
        // group 1 the material block
        const std::uint32_t frameOffsets[2] = {static_cast<std::uint32_t>(offset),
                                               static_cast<std::uint32_t>(chunk.lightingOffset)};
        const std::uint32_t materialOffset = static_cast<std::uint32_t>(offset + perDrawBytes);
        const WGPURenderPassEncoder pass = state.pass.Get();
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, chunk.frameGroup.Get(), 2, frameOffsets);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, materialGroup, 1, &materialOffset);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, webMesh.vertexBuffer.Get(), 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(pass, webMesh.indexBuffer.Get(), WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(pass, count, 1, first, 0, 0);
        NoteDraw(count, webMesh.vertexCount);
        // The in-frame UpdateMesh rule: this mesh's buffers are now referenced by the frame being recorded
        webMesh.lastDrawFrame = state.frameSerial;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Test and diagnostic hooks
    // -----------------------------------------------------------------------------------------------------------------

    bool WebGpuRenderer::CreatePipelineForTest(const BuiltInShader shader, const Common::RenderState& renderState)
    {
        std::lock_guard lock(_mutex);
        return _initialized && GetPipelineLocked(_state->shaders[ShaderIndex(shader)].get(), renderState) != nullptr;
    }

    std::uint32_t WebGpuRenderer::GetUserShaderCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->liveUserShaders.size());
    }

    std::uint32_t WebGpuRenderer::GetUserPipelineCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->userPipelineUse.size());
    }

    void WebGpuRenderer::SetMaxUserPipelinesForTest(const std::size_t limit)
    {
        std::lock_guard lock(_mutex);
        _state->maxUserPipelines = limit;
    }

    bool WebGpuRenderer::CreateUserPipelineForTest(Common::IShader* shader, const Common::RenderState& renderState)
    {
        std::lock_guard lock(_mutex);
        const WebGpuShader* user = _initialized ? FindUserShaderLocked(shader) : nullptr;
        return user != nullptr && GetPipelineLocked(user, renderState) != nullptr;
    }

    std::uint32_t WebGpuRenderer::GetPipelineKeyForTest(Common::IShader* shader, const Common::RenderState& renderState) const
    {
        std::lock_guard lock(_mutex);
        const WebGpuShader* found = nullptr;
        for (const auto& builtIn : _state->shaders)
        {
            if (builtIn != nullptr && shader == builtIn.get())
            {
                found = builtIn.get();
            }
        }
        if (found == nullptr)
        {
            found = FindUserShaderLocked(shader);
        }
        if (found == nullptr)
        {
            return 0xFFFFFFFFu;
        }
        PipelineKey key;
        key.shader = found->id;
        key.vertexLayout = found->IsUserShader() ? 1 : VertexLayoutId(found->kind);
        key.depthTest = renderState.depthTest;
        key.depthWrite = renderState.depthTest && renderState.depthWrite;
        key.cull = static_cast<std::uint8_t>(renderState.cull);
        key.blend = renderState.blend;
        key.additive = renderState.blend && renderState.additive;
        key.variant = (renderState.blend && !renderState.additive) ? static_cast<std::uint8_t>(renderState.variant) : 0;
        return key.Pack();
    }

    void WebGpuRenderer::SetFailPipelineCreationForTest(const bool fail)
    {
        std::lock_guard lock(_mutex);
        _state->failPipelineCreationForTest = fail;
    }

    std::uint32_t WebGpuRenderer::GetPipelineCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->pipelines.size());
    }

    std::uint32_t WebGpuRenderer::GetPipelineCreateAttemptCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->pipelineCreateAttempts;
    }

    std::uint32_t WebGpuRenderer::GetLiveMaterialCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->materials.size());
    }

    std::uint32_t WebGpuRenderer::GetBindGroupCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->materialGroups.size());
    }

    std::uint32_t WebGpuRenderer::GetRingChunkCountForTest() const
    {
        std::lock_guard lock(_mutex);
        return static_cast<std::uint32_t>(_state->chunks.size());
    }

    void WebGpuRenderer::SetRingChunkBytesForTest(const std::uint64_t bytes)
    {
        std::lock_guard lock(_mutex);
        _state->ringChunkBytes = bytes;
    }

    std::vector<std::uint8_t> WebGpuRenderer::GetLightingBlockForTest() const
    {
        std::lock_guard lock(_mutex);
        return std::vector<std::uint8_t>(_state->lighting.begin(), _state->lighting.end());
    }

    std::uint32_t WebGpuRenderer::GetUniformAlignmentForTest() const
    {
        std::lock_guard lock(_mutex);
        return _state->uniformAlignment;
    }

    bool WebGpuRenderer::SetMaterialTextureIdForTest(Common::IMaterial* material, const std::size_t slot, const std::uint64_t id)
    {
        std::lock_guard lock(_mutex);
        const auto it = _state->materials.find(material);
        if (it == _state->materials.end() || slot >= kSamplerCount)
        {
            return false;
        }
        it->second->SetTextureIdForTest(slot, id);
        return true;
    }

    bool WebGpuRenderer::GetMaterialBlockForTest(const Common::IMaterial* material, std::vector<std::uint8_t>& bytes) const
    {
        std::lock_guard lock(_mutex);
        const auto it = _state->materials.find(material);
        if (it == _state->materials.end())
        {
            return false;
        }
        const Gpu::BlockMaterial::Snapshot snapshot = it->second->TakeSnapshot();
        bytes.assign(snapshot.block.begin(), snapshot.block.begin() + snapshot.blockBytes);
        return true;
    }
}
