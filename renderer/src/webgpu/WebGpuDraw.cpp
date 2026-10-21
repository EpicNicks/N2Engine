#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "renderer/webgpu/WebGpuRenderer.hpp"

#include "WebGpuInternal.hpp"

#include <renderer/webgpu/WgslSources.hpp>

// Stage G3a of the WebGPU backend (#155): the built-in unlit shader, materials, the pipeline cache, the bind-group cache, the
// per-draw uniform ring and the draws. Written against webgpu.h (the C API) only.
//
// Resource layout (the same for every built-in shader, renderer/shaders/webgpu/*.wgsl):
//   group 0 (the frame): binding 0 the per-draw block (3 matrices, 192 bytes), a UNIFORM buffer with a DYNAMIC offset
//   group 1 (the material): binding 0 the material block (a dynamic offset), bindings 1-5 the base texture and the four
//   AuxTexture slots, bindings 6-10 their samplers (WGSL has no combined image sampler)
// The pipeline layout is the two groups, for all shaders, so a shader that reads fewer bindings (unlit reads 1 and 6 only) needs
// no layout of its own and the lit and text shaders (stage G3b) need none either.
//
// The uniform ring. A draw needs a per-draw block and a material block. Each draw takes the next slot of the current ring chunk
// (a uniform buffer; when it is full the frame moves to the next chunk, a new one if there is none) and fills it in a CPU copy
// (RingChunk::staging); EndFrame writes the used part of every chunk with ONE queue write per chunk, just before the submit.
// A slot is aligned to the device's minUniformBufferOffsetAlignment (a dynamic offset must be a multiple of it), so no draw of
// a frame overwrites another's block, and the next frame may reuse the whole chunk: its writes come after this frame's submit
// on the queue's timeline, so this frame has read its blocks by then. Nothing is mapped and nothing waits.
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
// Blending is stage G3b: blend and additive are in the key already, but G3a draws every pipeline opaque.

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
    using ShaderInterface::kMaterialBlockRange;
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

        // The unlit shader: one module holding vs_main and fs_main. A WGSL error is reported by the device (counted by the
        // error counters, which fail Initialize), not by a null module
        {
            auto shader = std::make_unique<WebGpuShader>(BuiltInShader::Unlit);
            WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
            source.code = WGPUStringView{Wgsl::kUnlitWgsl.data(), Wgsl::kUnlitWgsl.size()};
            WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine unlit shader");
            descriptor.nextInChain = &source.chain;
            shader->module.Reset(wgpuDeviceCreateShaderModule(state.device.Get(), &descriptor));
            if (!shader->module)
            {
                return Fail(InitStatus::Failed, "could not create the unlit shader module");
            }
            state.shaders[ShaderIndex(BuiltInShader::Unlit)] = std::move(shader);
        }

        // Group 0: the per-draw block
        {
            WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            entry.binding = 0;
            entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            entry.buffer.type = WGPUBufferBindingType_Uniform;
            entry.buffer.hasDynamicOffset = WGPU_TRUE;
            entry.buffer.minBindingSize = 0;
            WGPUBindGroupLayoutDescriptor descriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine frame group");
            descriptor.entryCount = 1;
            descriptor.entries = &entry;
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
        state.pipelineCreateAttempts = 0;
        state.pipelineLayout.Reset();
        state.materialLayout.Reset();
        state.frameLayout.Reset();
        for (auto& shader : state.shaders)
        {
            shader.reset();
        }
        state.materials.clear();
        for (auto& texture : state.defaults)
        {
            texture.reset();
        }
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
        }
        state.activeChunk = 0;
    }

    bool WebGpuRenderer::AddRingChunk(const std::uint64_t minimumSize)
    {
        State& state = *_state;
        // The size of a new chunk, a multiple of the alignment; dynamic offsets are 32-bit, so a chunk stays under 4 GiB
        const std::uint64_t size = AlignUp64(std::max(state.ringChunkBytes, minimumSize), state.uniformAlignment);
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

    WGPURenderPipeline WebGpuRenderer::GetPipelineLocked(const BuiltInShader kind, const RenderState& renderState)
    {
        State& state = *_state;
        const WebGpuShader* shader = state.shaders[ShaderIndex(kind)].get();
        if (shader == nullptr || !shader->IsValid() || !state.pipelineLayout)
        {
            return nullptr;
        }

        PipelineKey key;
        key.shader = static_cast<std::uint8_t>(kind);
        key.vertexLayout = VertexLayoutId(kind);
        key.depthTest = renderState.depthTest;
        // Nothing writes depth while the test is off (as OpenGL): a draw with the test off and the write on shares the pipeline
        // of the draw with both off
        key.depthWrite = renderState.depthTest && renderState.depthWrite;
        key.cull = static_cast<std::uint8_t>(renderState.cull);
        key.blend = renderState.blend;
        key.additive = renderState.blend && renderState.additive; // additive without blend is an opaque draw, as in GL
        const std::uint32_t packed = key.Pack();
        if (const auto it = state.pipelines.find(packed); it != state.pipelines.end())
        {
            return it->second.Get();
        }
        if (state.failedPipelines.count(packed) != 0)
        {
            return nullptr; // this key failed before (logged once): skipped without retrying
        }
        if (renderState.blend)
        {
            LogOnce("blending is not implemented yet (#155, stage G3b): draws that ask for it are drawn opaque");
        }

        // One interleaved vertex buffer of Common::Vertex (64 bytes); exactly the attributes the shader reads
        std::array<WGPUVertexAttribute, 5> attributes;
        std::uint32_t attributeCount = 0;
        for (const ShaderInterface::Attribute& attribute : ShaderInterface::Attributes(kind))
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

        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = GetColorFormat();
        target.blend = nullptr; // opaque (blending is stage G3b)
        target.writeMask = WGPUColorWriteMask_All;

        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = shader->module.Get();
        fragment.entryPoint = ToView("fs_main");
        fragment.targetCount = 1;
        fragment.targets = &target;

        WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = ToView("N2Engine pipeline");
        descriptor.layout = state.pipelineLayout.Get();
        descriptor.vertex.module = shader->module.Get();
        descriptor.vertex.entryPoint = ToView("vs_main");
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
        if (!state.failPipelineCreationForTest)
        {
            pipeline.Reset(wgpuDeviceCreateRenderPipeline(state.device.Get(), &descriptor));
        }
        if (!pipeline)
        {
            // Remember the failure (a negative cache entry) so every later draw with this key skips quietly; one line per key
            state.failedPipelines.insert(packed);
            LogOnce("could not create the render pipeline for key " + std::to_string(packed) +
                    ": draws with that state are skipped");
            return nullptr;
        }
        const WGPURenderPipeline raw = pipeline.Get();
        state.pipelines.emplace(packed, std::move(pipeline));
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
        static_cast<void>(shader); // the built-in shaders belong to the renderer
        return false;
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
        return false;
    }

    Common::IShader* WebGpuRenderer::GetStandardUnlitShader() const
    {
        std::lock_guard lock(_mutex);
        const WebGpuShader* shader = _state->shaders[ShaderIndex(BuiltInShader::Unlit)].get();
        if (!_initialized || shader == nullptr || !shader->IsValid())
        {
            return nullptr;
        }
        return const_cast<WebGpuShader*>(shader);
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
            LogOnce("CreateMaterial: only materials of this renderer's built-in shaders (the unlit one in stage G3a) exist; "
                    "returning null");
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
        static_cast<void>(lighting);
        static_cast<void>(cameraPosition);
        // Only the lit shader reads the lighting, and it is stage G3b: nothing to keep yet
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

        const WGPURenderPipeline pipeline = GetPipelineLocked(blockMaterial.kind, renderState);
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
                texture = state.defaults[i].get();
            }
            if (texture == nullptr)
            {
                return;
            }
            textures[i] = texture;
        }

        // This draw's room in the ring: the per-draw block, then the material block, each at an aligned offset
        const std::uint64_t perDrawBytes = AlignUp64(kPerDrawBlockSize, state.uniformAlignment);
        const std::uint64_t materialBytes = AlignUp64(kMaterialBlockRange, state.uniformAlignment);
        std::size_t chunkIndex = 0;
        std::uint64_t offset = 0;
        if (!AllocateRing(perDrawBytes + materialBytes, chunkIndex, offset))
        {
            return;
        }
        RingChunk& chunk = state.chunks[chunkIndex];

        // Row-major in, columns out: each matrix is transposed here, once (the WGSL reads the bytes as mat4x4<f32>)
        std::uint8_t* perDraw = chunk.staging.data() + offset;
        WriteTransposed(perDraw, modelMatrix);
        WriteTransposed(perDraw + 64, state.view);
        WriteTransposed(perDraw + 128, state.projection);
        std::memcpy(perDraw + perDrawBytes, snapshot.block.data(), snapshot.block.size());

        // Group 0: this chunk's per-draw range at a dynamic offset, made once per chunk
        if (!chunk.frameGroup)
        {
            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            entry.binding = 0;
            entry.buffer = chunk.buffer.Get();
            entry.offset = 0;
            entry.size = kPerDrawBlockSize;
            WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            descriptor.label = ToView("N2Engine frame group");
            descriptor.layout = state.frameLayout.Get();
            descriptor.entryCount = 1;
            descriptor.entries = &entry;
            chunk.frameGroup.Reset(wgpuDeviceCreateBindGroup(state.device.Get(), &descriptor));
            if (!chunk.frameGroup)
            {
                return;
            }
        }

        // Group 1: this chunk's material range at a dynamic offset and the five textures with their samplers. Immutable, so
        // draws with the same textures in the same chunk share one
        std::array<std::uint64_t, 1 + kSamplerCount> key{};
        key[0] = chunkIndex;
        for (std::size_t i = 0; i < kSamplerCount; ++i)
        {
            key[1 + i] = textures[i]->id;
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
            entries[0].size = kMaterialBlockRange;
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

        // Dynamic offsets, one per group (each group has one dynamic binding)
        const std::uint32_t frameOffset = static_cast<std::uint32_t>(offset);
        const std::uint32_t materialOffset = static_cast<std::uint32_t>(offset + perDrawBytes);
        const WGPURenderPassEncoder pass = state.pass.Get();
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, chunk.frameGroup.Get(), 1, &frameOffset);
        wgpuRenderPassEncoderSetBindGroup(pass, 1, materialGroup, 1, &materialOffset);
        wgpuRenderPassEncoderSetVertexBuffer(pass, 0, webMesh.vertexBuffer.Get(), 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderSetIndexBuffer(pass, webMesh.indexBuffer.Get(), WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
        wgpuRenderPassEncoderDrawIndexed(pass, count, 1, first, 0, 0);
        // The in-frame UpdateMesh rule: this mesh's buffers are now referenced by the frame being recorded
        webMesh.lastDrawFrame = state.frameSerial;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Test and diagnostic hooks
    // -----------------------------------------------------------------------------------------------------------------

    bool WebGpuRenderer::CreatePipelineForTest(const BuiltInShader shader, const Common::RenderState& renderState)
    {
        std::lock_guard lock(_mutex);
        return _initialized && GetPipelineLocked(shader, renderState) != nullptr;
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
        bytes.assign(snapshot.block.begin(), snapshot.block.end());
        return true;
    }
}
