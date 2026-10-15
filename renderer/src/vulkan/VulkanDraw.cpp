#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

#include "renderer/vulkan/VulkanRenderer.hpp"

#include "VulkanDraw.hpp"
#include "VulkanResources.hpp"

// Stages B, C and D of the Vulkan backend (#43): the three built-in shaders (unlit, lit, text; unlit and lit are handed out
// and drawn with so far, text is stage E), materials, the pipeline layout and cache, the per-draw uniform ring, descriptor
// sets, scene lighting, and DrawMesh / IndexRange / DrawObjects, with blending (stage D) exactly as OpenGL's ApplyRenderState:
// blend -> SRC_ALPHA / ONE_MINUS_SRC_ALPHA (additive: SRC_ALPHA / ONE) for colour AND alpha, depth write only while the draw also
// depth-tests, and additive without blend an opaque draw.
//
// Shaders: GLSL 450 under renderer/shaders/vulkan/, compiled to SPIR-V by glslangValidator or glslc when the renderer is
// BUILT (renderer/CMakeLists.txt) and embedded as generated headers; nothing generated is committed. Without either tool
// the build still succeeds (N2ENGINE_VULKAN_HAS_SPIRV=0): there are then no shader modules and nothing draws.
//
// Interface (VulkanShaderInterface.hpp is the table; the GLSL declares the same):
//   set 0 (the frame):    binding 0 PerDraw (model, view, projection: UNIFORM_BUFFER_DYNAMIC, an offset per draw in the ring),
//                         binding 1 Lighting (UNIFORM_BUFFER_DYNAMIC: a copy of the scene lighting block in the ring, below)
//   set 1 (the material): binding 0 Material block (UNIFORM_BUFFER_DYNAMIC, an offset per draw in the ring),
//                         bindings 1-5 COMBINED_IMAGE_SAMPLER (the base texture, then the AuxTexture slots in enum order;
//                         an absent texture is a renderer-owned 1x1 default)
// One pipeline layout serves all three shaders.
//
// SCENE LIGHTING (stage C). UpdateSceneLighting builds the 752-byte std140 block (VulkanLighting.hpp) into a CPU copy and
// bumps a version; it touches no GPU memory. The first LIT draw after an update copies the block into the current frame
// slot's ring and every lit draw names that copy with the dynamic offset of set 0's binding 1 (one copy per ring chunk per
// version). So, as on OpenGL, a draw uses the lighting set by the last UpdateSceneLighting BEFORE it, an update after a draw
// does not change it, and the next frame's update does not touch a frame in flight: the copies live in the ring, which is
// rewritten only after the slot's fence wait, like the per-draw data. (The stage B single lighting buffer, read by both frame
// slots, is gone; the alternative of one buffer per slot would need a ring copy for a second update in a frame anyway.)
// Before the first update the block holds SceneLightingData{} seen from the origin, as the software renderer's members do.
//
// A draw is immediate: DrawMesh writes the draw's model/view/projection and a snapshot of the material block into the
// current frame slot's ring (a host-visible, persistently mapped uniform buffer, aligned to
// minUniformBufferOffsetAlignment), finds or makes the descriptor sets, and records bind + draw into the frame's command
// buffer. A change to the material after DrawMesh therefore does not affect that draw (as on OpenGL).
//
// The ring is per frame slot and CHAINED: when a chunk is full the frame takes the next (twice as big) one, so any number
// of draws works; chunks persist and the slot's frame starts again from the first chunk. Slot S's chunks are written only
// after BeginFrame waited for slot S's fence. The descriptor sets come from per-slot pools that BeginFrame resets after the
// same wait. Nothing a pending frame may read is ever rewritten.
//
// Pipelines are cached by (shader, RenderState, wireframe) and made lazily on first use, under the renderer's lock, with
// a dynamic viewport and scissor so resizing the target never invalidates them. Draws reach the target in submission order
// (the renderer never reorders), and SetViewProjection mid-frame (the UI pass after the scene) affects only the draws after it:
// each draw copies the matrices current when it was recorded into its own ring block. Clip space: the vertex shaders remap z from
// OpenGL's -w..w to Vulkan's 0..w, and BeginFrame's viewport has a negative height so y stays up as in GL; the image is then
// upright as in GL, so GL's counter-clockwise front faces stay VK_FRONT_FACE_COUNTER_CLOCKWISE.

#ifndef N2ENGINE_VULKAN_HAS_SPIRV
#define N2ENGINE_VULKAN_HAS_SPIRV 0
#endif

#if N2ENGINE_VULKAN_HAS_SPIRV
#include "vulkan_shaders/lit_frag_spirv.h"
#include "vulkan_shaders/lit_vert_spirv.h"
#include "vulkan_shaders/text_frag_spirv.h"
#include "vulkan_shaders/text_vert_spirv.h"
#include "vulkan_shaders/unlit_frag_spirv.h"
#include "vulkan_shaders/unlit_vert_spirv.h"
#endif

using namespace Renderer::Vulkan;
using Renderer::Common::CullMode;
using Renderer::Common::IMaterial;
using Renderer::Common::IMesh;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;
using Renderer::Common::RenderState;
using Renderer::Common::TextureFilter;
using Renderer::Common::TextureOptions;
using Renderer::Common::TextureWrap;
using namespace Renderer::Vulkan::ShaderInterface;

namespace
{
    constexpr std::uint32_t SetsPerPool = 256;
    constexpr VkDeviceSize FirstChunkSize = 256 * 1024;
    constexpr VkDeviceSize MaxChunkSize = 64ull * 1024 * 1024;

    constexpr std::size_t ShaderIndex(const BuiltInShader shader)
    {
        return static_cast<std::size_t>(shader);
    }

    VkDeviceSize AlignUp(const VkDeviceSize value, const VkDeviceSize alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    template <typename Handle>
    std::uint64_t HandleValue(const Handle handle)
    {
        if constexpr (std::is_pointer_v<Handle>)
        {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
        }
        else
        {
            return static_cast<std::uint64_t>(handle);
        }
    }

    VkFormat AttributeFormat(const std::uint32_t components)
    {
        switch (components)
        {
        case 1: return VK_FORMAT_R32_SFLOAT;
        case 2: return VK_FORMAT_R32G32_SFLOAT;
        case 3: return VK_FORMAT_R32G32B32_SFLOAT;
        default: return VK_FORMAT_R32G32B32A32_SFLOAT;
        }
    }

    const char* TypeName(const UniformType type)
    {
        switch (type)
        {
        case UniformType::Int: return "int";
        case UniformType::Float: return "float";
        case UniformType::Vec2: return "vec2";
        case UniformType::Vec3: return "vec3";
        case UniformType::Vec4: return "vec4";
        case UniformType::Mat4: return "mat4";
        case UniformType::Struct: return "struct";
        }
        return "?";
    }

#if N2ENGINE_VULKAN_HAS_SPIRV
    struct Spirv
    {
        const std::uint32_t* words;
        std::size_t bytes;
    };

    Spirv VertexSpirv(const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::Unlit: return {kSpirv_unlit_vert, sizeof(kSpirv_unlit_vert)};
        case BuiltInShader::Lit: return {kSpirv_lit_vert, sizeof(kSpirv_lit_vert)};
        case BuiltInShader::Text: return {kSpirv_text_vert, sizeof(kSpirv_text_vert)};
        }
        return {nullptr, 0};
    }

    Spirv FragmentSpirv(const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::Unlit: return {kSpirv_unlit_frag, sizeof(kSpirv_unlit_frag)};
        case BuiltInShader::Lit: return {kSpirv_lit_frag, sizeof(kSpirv_lit_frag)};
        case BuiltInShader::Text: return {kSpirv_text_frag, sizeof(kSpirv_text_frag)};
        }
        return {nullptr, 0};
    }

    VkShaderModule MakeModule(VkDevice device, const Spirv& code)
    {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = code.bytes;
        info.pCode = code.words;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        return module;
    }
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// VulkanMaterial
// ---------------------------------------------------------------------------------------------------------------------

VulkanMaterial::VulkanMaterial(const BuiltInShader shaderKind, IShader* owner, ITexture* baseTexture, Warn warn,
                               TextureIdOf textureIdOf)
    : kind(shaderKind), shader(owner), _warn(std::move(warn)), _textureIdOf(std::move(textureIdOf)), _texture(baseTexture)
{
    _textureId = (_textureIdOf && baseTexture != nullptr) ? _textureIdOf(baseTexture) : 0;
    // The OpenGL defaults, from the table (ShaderInterface::MaterialDefaults): OpenGLMaterial's constructor sets uAlbedo
    // white, OpenGLRenderer::CreateMaterial the rest per shader (for lit: uMetallic 0, uSmoothness 0.5, uAlphaCutoff 0,
    // uEmissive 0, the four uHas*Texture flags 0, uOcclusionStrength 1, uNormalScale 1, uPbr 0). All of them go through the
    // name table, so the type of each entry decides how it is stored
    for (const MaterialDefault& entry : MaterialDefaults(kind))
    {
        const Uniform* uniform = FindMaterialUniform(kind, entry.name);
        if (uniform == nullptr)
        {
            continue;
        }
        if (uniform->type == UniformType::Int)
        {
            const auto value = static_cast<std::int32_t>(entry.values[0]);
            std::memcpy(_block.data() + uniform->offset, &value, sizeof(value));
        }
        else
        {
            const std::uint32_t floats = uniform->type == UniformType::Vec4 ? 4 : uniform->type == UniformType::Vec3 ? 3
                : uniform->type == UniformType::Vec2                                                                 ? 2
                                                                                                                      : 1;
            WriteFloats(uniform->offset, entry.values.data(), floats);
        }
    }
    // uHasTexture: a texture was given at creation (SetTexture does not touch it, in either backend)
    if (const Uniform* hasTexture = FindMaterialUniform(kind, "uHasTexture"))
    {
        const std::int32_t value = baseTexture != nullptr ? 1 : 0;
        std::memcpy(_block.data() + hasTexture->offset, &value, sizeof(value));
    }
}

const Uniform* VulkanMaterial::Find(const std::string& name, const UniformType expected, const char* setter) const
{
    const Uniform* uniform = FindMaterialUniform(kind, name);
    if (uniform == nullptr)
    {
        if (_warn)
        {
            _warn("material uniform '" + name + "' is not in the " + std::string(ShaderName(kind)) +
                  " shader's material block: ignored");
        }
        return nullptr;
    }
    if (uniform->type != expected)
    {
        if (_warn)
        {
            _warn("material uniform '" + name + "' is a " + TypeName(uniform->type) + ", so " + setter +
                  " does not apply to it: ignored");
        }
        return nullptr;
    }
    return uniform;
}

void VulkanMaterial::WriteFloats(const std::uint32_t offset, const float* values, const std::uint32_t count)
{
    if (offset + count * sizeof(float) <= _block.size())
    {
        std::memcpy(_block.data() + offset, values, count * sizeof(float));
    }
}

void VulkanMaterial::SetInt(const std::string& name, const int value)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Int, "SetInt"))
    {
        const std::int32_t stored = value;
        std::memcpy(_block.data() + uniform->offset, &stored, sizeof(stored));
    }
}

void VulkanMaterial::SetFloat(const std::string& name, const float value)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Float, "SetFloat"))
    {
        WriteFloats(uniform->offset, &value, 1);
    }
}

void VulkanMaterial::SetVec2(const std::string& name, const float x, const float y)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec2, "SetVec2"))
    {
        const float values[2] = {x, y};
        WriteFloats(uniform->offset, values, 2);
    }
}

void VulkanMaterial::SetVec2(const std::string& name, N2Engine::Math::Vector2& value)
{
    SetVec2(name, value.x, value.y);
}

void VulkanMaterial::SetVec3(const std::string& name, const float x, const float y, const float z)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec3, "SetVec3"))
    {
        const float values[3] = {x, y, z};
        WriteFloats(uniform->offset, values, 3);
    }
}

void VulkanMaterial::SetVec3(const std::string& name, N2Engine::Math::Vector3& value)
{
    SetVec3(name, value.x, value.y, value.z);
}

void VulkanMaterial::SetVec4(const std::string& name, const float x, const float y, const float z, const float w)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec4, "SetVec4"))
    {
        const float values[4] = {x, y, z, w};
        WriteFloats(uniform->offset, values, 4);
    }
}

void VulkanMaterial::SetVec4(const std::string& name, N2Engine::Math::Vector4& value)
{
    SetVec4(name, value.x, value.y, value.z, value.w);
}

void VulkanMaterial::SetColor(const std::string& name, const float r, const float g, const float b, const float a)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec4, "SetColor"))
    {
        const float values[4] = {r, g, b, a};
        WriteFloats(uniform->offset, values, 4);
    }
}

void VulkanMaterial::SetTexture(ITexture* texture)
{
    // The id is asked for before this material's lock is taken (the resolver takes its own leaf lock)
    const std::uint64_t id = (_textureIdOf && texture != nullptr) ? _textureIdOf(texture) : 0;
    std::lock_guard lock(_mutex);
    _texture = texture;
    _textureId = id;
}

void VulkanMaterial::SetAuxTexture(const Common::AuxTexture which, ITexture* texture)
{
    const std::uint64_t id = (_textureIdOf && texture != nullptr) ? _textureIdOf(texture) : 0;
    std::lock_guard lock(_mutex);
    if (const auto slot = static_cast<std::size_t>(which); slot < _aux.size())
    {
        _aux[slot] = texture;
        _auxIds[slot] = id;
    }
}

ITexture* VulkanMaterial::GetAuxTexture(const Common::AuxTexture which) const
{
    std::lock_guard lock(_mutex);
    const auto slot = static_cast<std::size_t>(which);
    return slot < _aux.size() ? _aux[slot] : nullptr;
}

ITexture* VulkanMaterial::GetTexture() const
{
    std::lock_guard lock(_mutex);
    return _texture;
}

void VulkanMaterial::SetTextureIdForTest(const std::size_t slot, const std::uint64_t id)
{
    std::lock_guard lock(_mutex);
    if (slot == 0)
    {
        _textureId = id;
    }
    else if (slot - 1 < _auxIds.size())
    {
        _auxIds[slot - 1] = id;
    }
}

VulkanMaterial::Snapshot VulkanMaterial::TakeSnapshot() const
{
    std::lock_guard lock(_mutex);
    Snapshot snapshot;
    snapshot.block = _block;
    snapshot.textures[0] = _texture;
    snapshot.textureIds[0] = _textureId;
    for (std::size_t i = 0; i < _aux.size(); ++i)
    {
        snapshot.textures[1 + i] = _aux[i];
        snapshot.textureIds[1 + i] = _auxIds[i];
    }
    return snapshot;
}

// ---------------------------------------------------------------------------------------------------------------------
// Creation and destruction of the draw state
// ---------------------------------------------------------------------------------------------------------------------

bool VulkanRenderer::HasBuiltInShaders()
{
    return N2ENGINE_VULKAN_HAS_SPIRV != 0;
}

bool VulkanRenderer::CreateDrawResources()
{
    DrawState& draw = *_draw;
    draw.uniformAlignment = std::max<VkDeviceSize>(1, _deviceProperties.limits.minUniformBufferOffsetAlignment);

    // Set 0: the frame
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; // the lighting copy, an offset per draw
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &info, nullptr, &draw.frameSetLayout) != VK_SUCCESS)
        {
            draw.frameSetLayout = VK_NULL_HANDLE;
            return Fail(InitStatus::Failed, "vkCreateDescriptorSetLayout failed (the frame set)");
        }
    }

    // Set 1: the material
    {
        std::array<VkDescriptorSetLayoutBinding, 1 + kSamplerCount> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (std::uint32_t i = 0; i < kSamplerCount; ++i)
        {
            bindings[1 + i].binding = kFirstSamplerBinding + i;
            bindings[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1 + i].descriptorCount = 1;
            bindings[1 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &info, nullptr, &draw.materialSetLayout) != VK_SUCCESS)
        {
            draw.materialSetLayout = VK_NULL_HANDLE;
            return Fail(InitStatus::Failed, "vkCreateDescriptorSetLayout failed (the material set)");
        }
    }

    {
        const VkDescriptorSetLayout layouts[2] = {draw.frameSetLayout, draw.materialSetLayout};
        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 2;
        info.pSetLayouts = layouts;
        if (vkCreatePipelineLayout(_device, &info, nullptr, &draw.pipelineLayout) != VK_SUCCESS)
        {
            draw.pipelineLayout = VK_NULL_HANDLE;
            return Fail(InitStatus::Failed, "vkCreatePipelineLayout failed");
        }
    }

    // The built-in shaders. Without SPIR-V (no compiler found at build time) they exist but have no modules, and
    // nothing draws
    for (std::size_t i = 0; i < kBuiltInShaderCount; ++i)
    {
        const auto kind = static_cast<BuiltInShader>(i);
        auto shader = std::make_unique<VulkanShader>(kind);
#if N2ENGINE_VULKAN_HAS_SPIRV
        shader->vertexModule = MakeModule(_device, VertexSpirv(kind));
        shader->fragmentModule = MakeModule(_device, FragmentSpirv(kind));
        if (!shader->IsValid())
        {
            if (shader->vertexModule != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(_device, shader->vertexModule, nullptr);
            }
            if (shader->fragmentModule != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(_device, shader->fragmentModule, nullptr);
            }
            return Fail(InitStatus::Failed, "vkCreateShaderModule failed for the " + std::string(ShaderName(kind)) +
                                                " shader");
        }
#endif
        draw.shaders[i] = std::move(shader);
    }

    // The 1x1 defaults for an absent texture: base white, emissive black (adds nothing), occlusion white (no occlusion),
    // normal flat (0.5, 0.5, 1), metallic-roughness white (the factors alone)
    {
        static constexpr std::uint8_t pixels[kSamplerCount][4] = {
            {255, 255, 255, 255}, {0, 0, 0, 255}, {255, 255, 255, 255}, {128, 128, 255, 255}, {255, 255, 255, 255}};
        TextureOptions options;
        options.filter = TextureFilter::Nearest;
        options.wrap = TextureWrap::ClampToEdge;
        options.mipmaps = false;
        options.srgb = false;
        for (std::uint32_t i = 0; i < kSamplerCount; ++i)
        {
            draw.defaults[i] = MakeTextureLocked(pixels[i], 1, 1, 4, options);
            if (!draw.defaults[i])
            {
                return Fail(InitStatus::Failed, "could not make a default texture");
            }
            ++draw.internalAllocations;
        }
    }

    for (FrameDrawState& frame : draw.frames)
    {
        frame = {};
    }
    return true;
}

void VulkanRenderer::DestroyDrawResources()
{
    DrawState& draw = *_draw;
    if (_device != VK_NULL_HANDLE)
    {
        for (const auto& entry : draw.pipelines)
        {
            vkDestroyPipeline(_device, entry.second, nullptr);
        }
        draw.pipelines.clear();

        for (FrameDrawState& frame : draw.frames)
        {
            for (const VkDescriptorPool pool : frame.pools)
            {
                vkDestroyDescriptorPool(_device, pool, nullptr); // frees its sets
            }
            frame.pools.clear();
            frame.materialSets.clear();
            for (RingChunk& chunk : frame.chunks)
            {
                if (chunk.buffer != VK_NULL_HANDLE && _resources->allocator != nullptr)
                {
                    vmaDestroyBuffer(_resources->allocator, chunk.buffer, chunk.allocation);
                }
            }
            frame.chunks.clear();
        }

        if (draw.pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(_device, draw.pipelineLayout, nullptr);
        }
        if (draw.frameSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(_device, draw.frameSetLayout, nullptr);
        }
        if (draw.materialSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(_device, draw.materialSetLayout, nullptr);
        }
        for (const std::unique_ptr<VulkanShader>& shader : draw.shaders)
        {
            if (!shader)
            {
                continue;
            }
            if (shader->vertexModule != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(_device, shader->vertexModule, nullptr);
            }
            if (shader->fragmentModule != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(_device, shader->fragmentModule, nullptr);
            }
        }
        for (const std::unique_ptr<VulkanTexture>& texture : draw.defaults)
        {
            if (!texture)
            {
                continue;
            }
            if (texture->view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(_device, texture->view, nullptr);
            }
            if (texture->image != VK_NULL_HANDLE && _resources->allocator != nullptr)
            {
                vmaDestroyImage(_resources->allocator, texture->image, texture->allocation);
            }
        }
    }
    _draw = std::make_unique<DrawState>(); // the materials (CPU objects) go too: Shutdown frees every live one
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame state: the ring, the descriptor pools
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::BeginDrawFrame(const uint32_t slot)
{
    // Runs after BeginFrame waited for this slot's fence: the slot's earlier frame has finished, so its ring chunks and
    // descriptor sets are not read any more
    static_assert(MAX_FRAMES_IN_FLIGHT == 2, "DrawState::frames has one entry per frame slot");
    FrameDrawState& frame = _draw->frames[slot];
    for (const VkDescriptorPool pool : frame.pools)
    {
        vkResetDescriptorPool(_device, pool, 0);
    }
    frame.activePool = 0;
    frame.activePoolSets = 0;
    frame.materialSets.clear();
    for (RingChunk& chunk : frame.chunks)
    {
        chunk.used = 0;
        chunk.frameSet = VK_NULL_HANDLE;
        chunk.lightingVersion = 0; // the lighting copy of an earlier frame is not this frame's
        chunk.lightingOffset = 0;
    }
    frame.activeChunk = 0;
}

void VulkanRenderer::EndDrawFrame(const uint32_t slot)
{
    // Host writes to non-coherent memory must be flushed before the submit (a no-op on coherent memory)
    FrameDrawState& frame = _draw->frames[slot];
    for (RingChunk& chunk : frame.chunks)
    {
        if (chunk.used > 0)
        {
            vmaFlushAllocation(_resources->allocator, chunk.allocation, 0, VK_WHOLE_SIZE);
        }
    }
}

bool VulkanRenderer::AddRingChunk(const uint32_t slot, const VkDeviceSize minimumSize)
{
    FrameDrawState& frame = _draw->frames[slot];
    VkDeviceSize size = frame.chunks.empty() ? FirstChunkSize : std::min(frame.chunks.back().size * 2, MaxChunkSize);
    size = std::max(size, AlignUp(minimumSize, 4096));

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    RingChunk chunk;
    VmaAllocationInfo resultInfo{};
    if (vmaCreateBuffer(_resources->allocator, &info, &allocationInfo, &chunk.buffer, &chunk.allocation, &resultInfo) !=
            VK_SUCCESS ||
        resultInfo.pMappedData == nullptr)
    {
        if (chunk.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(_resources->allocator, chunk.buffer, chunk.allocation);
        }
        std::cerr << "Vulkan renderer: could not allocate a uniform ring buffer of " << size << " bytes" << std::endl;
        return false;
    }
    chunk.mapped = static_cast<std::uint8_t*>(resultInfo.pMappedData);
    chunk.size = size;
    chunk.used = 0;
    frame.chunks.push_back(chunk);
    ++_draw->internalAllocations;
    return true;
}

bool VulkanRenderer::AllocateRing(const uint32_t slot, const VkDeviceSize bytes, std::size_t& chunkIndex,
                                  VkDeviceSize& offset)
{
    FrameDrawState& frame = _draw->frames[slot];
    for (;;)
    {
        if (frame.activeChunk < frame.chunks.size())
        {
            RingChunk& chunk = frame.chunks[frame.activeChunk];
            const VkDeviceSize start = AlignUp(chunk.used, _draw->uniformAlignment);
            if (start + bytes <= chunk.size)
            {
                chunk.used = start + bytes;
                chunkIndex = frame.activeChunk;
                offset = start;
                return true;
            }
            ++frame.activeChunk; // full: the next chunk (kept from an earlier frame, or a new one)
            continue;
        }
        if (!AddRingChunk(slot, bytes))
        {
            return false;
        }
    }
}

VkDescriptorSet VulkanRenderer::AllocateSet(const uint32_t slot, const VkDescriptorSetLayout layout)
{
    FrameDrawState& frame = _draw->frames[slot];
    // The pools are sized for SetsPerPool sets of either layout, so a pool that has handed out that many is full: move on
    // (to a pool kept from an earlier frame, or a new one). Counting ourselves means a pool is never asked for more than
    // it holds
    if (frame.pools.empty() || frame.activePoolSets >= SetsPerPool)
    {
        if (!frame.pools.empty() && frame.activePool + 1 < frame.pools.size())
        {
            ++frame.activePool;
            frame.activePoolSets = 0;
        }
        else
        {
            // A frame set holds two dynamic uniform buffers (per-draw, lighting), a material set one: 2 per set covers either
            const std::array<VkDescriptorPoolSize, 2> sizes = {{
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2 * SetsPerPool},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSamplerCount * SetsPerPool},
            }};
            VkDescriptorPoolCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.maxSets = SetsPerPool;
            info.poolSizeCount = static_cast<uint32_t>(sizes.size());
            info.pPoolSizes = sizes.data();
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(_device, &info, nullptr, &pool) != VK_SUCCESS)
            {
                std::cerr << "Vulkan renderer: could not create a descriptor pool" << std::endl;
                return VK_NULL_HANDLE;
            }
            frame.pools.push_back(pool);
            frame.activePool = frame.pools.size() - 1;
            frame.activePoolSets = 0;
        }
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = frame.pools[frame.activePool];
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(_device, &allocateInfo, &set) != VK_SUCCESS)
    {
        std::cerr << "Vulkan renderer: could not allocate a descriptor set" << std::endl;
        return VK_NULL_HANDLE;
    }
    ++frame.activePoolSets;
    return set;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipelines
// ---------------------------------------------------------------------------------------------------------------------

VkPipeline VulkanRenderer::GetPipelineLocked(const BuiltInShader shaderKind, const RenderState& state, const bool wireframe)
{
    DrawState& draw = *_draw;
    const VulkanShader* shader = draw.shaders[ShaderIndex(shaderKind)].get();
    if (shader == nullptr || !shader->IsValid() || draw.pipelineLayout == VK_NULL_HANDLE || _renderPass == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }
    if (wireframe && !draw.wireframeSupported)
    {
        return VK_NULL_HANDLE;
    }

    PipelineKey key;
    key.shader = static_cast<std::uint8_t>(shaderKind);
    key.depthTest = state.depthTest;
    key.depthWrite = state.depthWrite;
    key.cull = static_cast<std::uint8_t>(state.cull);
    key.blend = state.blend;
    key.additive = state.blend && state.additive; // GL: additive without blend is an opaque draw, so it shares the opaque pipeline
    key.wireframe = wireframe;
    const std::uint32_t packed = key.Pack();
    if (const auto it = draw.pipelines.find(packed); it != draw.pipelines.end())
    {
        return it->second;
    }
    if (draw.failedPipelines.count(packed) != 0)
    {
        return VK_NULL_HANDLE; // this key failed before (logged once): skipped without retrying
    }

    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shader->vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shader->fragmentModule;
    stages[1].pName = "main";

    // One interleaved binding of Common::Vertex (64 bytes); exactly the attributes the shader reads
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Common::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const Attribute& attribute : Attributes(shaderKind))
    {
        VkVertexInputAttributeDescription description{};
        description.location = attribute.location;
        description.binding = 0;
        description.format = AttributeFormat(attribute.components);
        description.offset = attribute.offset;
        attributes.push_back(description);
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1; // dynamic
    viewportState.scissorCount = 1;  // dynamic

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterization.cullMode = state.cull == CullMode::Back    ? VK_CULL_MODE_BACK_BIT
        : state.cull == CullMode::Front                      ? VK_CULL_MODE_FRONT_BIT
                                                             : VK_CULL_MODE_NONE;
    // GL's front faces are counter-clockwise as seen with y up. The negative-height viewport makes the image y-up as in GL,
    // and Vulkan judges the winding in that image (window coordinates, y down), so a GL front face is counter-clockwise
    // there too. (Without the flip, GL's projection would give an upside-down image and the winding would need to be
    // clockwise.)
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.depthBiasEnable = VK_FALSE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // GL: depth function LESS; nothing writes depth while the test is off
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = state.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = (state.depthTest && state.depthWrite) ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    // Blending exactly as OpenGL's ApplyRenderState: glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA) sets
    // the SAME factors for colour and alpha, equation ADD, so the target's alpha is blended too. Additive only counts with blend.
    VkPipelineColorBlendAttachmentState attachment{};
    if (state.blend)
    {
        const VkBlendFactor destinationFactor = state.additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = destinationFactor;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstAlphaBlendFactor = destinationFactor;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    else
    {
        attachment.blendEnable = VK_FALSE;
    }
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &attachment;

    const VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = draw.pipelineLayout;
    info.renderPass = _renderPass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    ++draw.pipelineCreateAttempts;
    const VkResult result = draw.failPipelineCreationForTest
        ? VK_ERROR_INITIALIZATION_FAILED
        : vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        // Remember the failure (a negative cache entry) so every later draw with this key skips quietly instead of
        // retrying and logging again; one line per key
        draw.failedPipelines.insert(packed);
        LogOnce("vkCreateGraphicsPipelines failed (result " + std::to_string(static_cast<int>(result)) +
                ") for pipeline key " + std::to_string(packed) + ": draws with that state are skipped");
        return VK_NULL_HANDLE;
    }
    draw.pipelines.emplace(packed, pipeline);
    return pipeline;
}

bool VulkanRenderer::CreatePipelineForTest(const BuiltInShader shader, const RenderState& state, const bool wireframe)
{
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost)
    {
        return false;
    }
    return GetPipelineLocked(shader, state, wireframe) != VK_NULL_HANDLE;
}

uint32_t VulkanRenderer::GetPipelineCountForTest() const
{
    std::lock_guard lock(_mutex);
    return static_cast<uint32_t>(_draw->pipelines.size());
}

bool VulkanRenderer::IsWireframeSupportedForTest() const
{
    std::lock_guard lock(_mutex);
    return _draw->wireframeSupported;
}

void VulkanRenderer::SetWireframeUnavailableForTest(const bool unavailable)
{
    std::lock_guard lock(_mutex);
    _draw->wireframeUnavailableForTest = unavailable;
    if (_initialized && unavailable)
    {
        // Like the real feature-absent path at Initialize: support is off, and wireframe that was already asked for is
        // turned off with the shared line (once), so its draws come out solid instead of being dropped without a word
        _draw->wireframeSupported = false;
        if (_draw->wireframe)
        {
            LogOnce(kWireframeIgnoredMessage);
            _draw->wireframe = false;
        }
    }
}

void VulkanRenderer::SetFailPipelineCreationForTest(const bool fail)
{
    std::lock_guard lock(_mutex);
    _draw->failPipelineCreationForTest = fail;
}

uint32_t VulkanRenderer::GetPipelineCreateAttemptCountForTest() const
{
    std::lock_guard lock(_mutex);
    return _draw->pipelineCreateAttempts;
}

bool VulkanRenderer::SetMaterialTextureIdForTest(IMaterial* material, const std::size_t slot, const std::uint64_t id)
{
    std::lock_guard lock(_mutex);
    const auto it = _draw->materials.find(material);
    if (it == _draw->materials.end() || slot > 4)
    {
        return false;
    }
    it->second->SetTextureIdForTest(slot, id);
    return true;
}

uint64_t VulkanRenderer::GetTextureIdForTest(const ITexture* texture) const
{
    std::lock_guard lock(_mutex);
    return TextureIdLocked(texture);
}

uint64_t VulkanRenderer::TextureIdLocked(const ITexture* texture) const
{
    std::lock_guard idLock(_draw->textureIdMutex);
    const auto it = _draw->textureIds.find(texture);
    return it != _draw->textureIds.end() ? it->second : 0;
}

uint32_t VulkanRenderer::GetLiveMaterialCountForTest() const
{
    std::lock_guard lock(_mutex);
    return static_cast<uint32_t>(_draw->materials.size());
}

uint32_t VulkanRenderer::GetRingChunkCountForTest() const
{
    std::lock_guard lock(_mutex);
    std::size_t count = 0;
    for (const FrameDrawState& frame : _draw->frames)
    {
        count += frame.chunks.size();
    }
    return static_cast<uint32_t>(count);
}

IShader* VulkanRenderer::GetBuiltInShaderForTest(const BuiltInShader shader) const
{
    std::lock_guard lock(_mutex);
    const VulkanShader* found = _draw->shaders[ShaderIndex(shader)].get();
    return (found != nullptr && found->IsValid()) ? const_cast<VulkanShader*>(found) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shaders and materials
// ---------------------------------------------------------------------------------------------------------------------

IShader* VulkanRenderer::CreateShaderProgram(const char* vertexSource, const char* fragmentSource)
{
    static_cast<void>(vertexSource);
    static_cast<void>(fragmentSource);
    LogOnce("custom GLSL shaders are not supported by the Vulkan backend in v1 (the three built-in shaders only): "
            "CreateShaderProgram returns null");
    return nullptr;
}

void VulkanRenderer::UseShaderProgram(IShader* shader)
{
    static_cast<void>(shader); // a draw takes its shader from its material
}

bool VulkanRenderer::DestroyShaderProgram(IShader* shader)
{
    static_cast<void>(shader); // the built-ins belong to the renderer; nullptr and foreign shaders are not its to destroy
    return false;
}

bool VulkanRenderer::IsValidShader(IShader* shader) const
{
    if (shader == nullptr)
    {
        return false;
    }
    std::lock_guard lock(_mutex);
    for (const std::unique_ptr<VulkanShader>& builtIn : _draw->shaders)
    {
        if (builtIn && builtIn.get() == shader)
        {
            return builtIn->IsValid();
        }
    }
    return false; // compared only: a foreign pointer is never dereferenced
}

IShader* VulkanRenderer::GetStandardUnlitShader() const
{
    {
        std::lock_guard lock(_mutex);
        if (const VulkanShader* shader = _draw->shaders[ShaderIndex(BuiltInShader::Unlit)].get();
            shader != nullptr && shader->IsValid())
        {
            return const_cast<VulkanShader*>(shader);
        }
    }
    if (!HasBuiltInShaders())
    {
        LogOnce("the built-in shaders were not compiled into this build (neither glslangValidator nor glslc was found when "
                "CMake configured the renderer): the unlit shader is not available and nothing will draw");
    }
    return nullptr;
}

IShader* VulkanRenderer::GetStandardLitShader() const
{
    {
        std::lock_guard lock(_mutex);
        if (const VulkanShader* shader = _draw->shaders[ShaderIndex(BuiltInShader::Lit)].get();
            shader != nullptr && shader->IsValid())
        {
            return const_cast<VulkanShader*>(shader);
        }
    }
    if (!HasBuiltInShaders())
    {
        LogOnce("the built-in shaders were not compiled into this build (neither glslangValidator nor glslc was found when "
                "CMake configured the renderer): the lit shader is not available and nothing will draw");
    }
    return nullptr;
}

IMaterial* VulkanRenderer::CreateMaterial(IShader* shader)
{
    return CreateMaterial(shader, nullptr);
}

IMaterial* VulkanRenderer::CreateMaterial(IShader* shader, ITexture* texture)
{
    if (shader == nullptr)
    {
        return nullptr;
    }
    std::lock_guard lock(_mutex);
    if (!_initialized || _deviceLost)
    {
        return nullptr;
    }
    // Unlit and lit have materials; the text shader's are stage E. A foreign shader is only compared with the built-ins,
    // never dereferenced
    const VulkanShader* chosen = nullptr;
    for (const BuiltInShader candidate : {BuiltInShader::Unlit, BuiltInShader::Lit})
    {
        const VulkanShader* builtIn = _draw->shaders[ShaderIndex(candidate)].get();
        if (builtIn != nullptr && builtIn->IsValid() && shader == builtIn)
        {
            chosen = builtIn;
        }
    }
    if (chosen == nullptr)
    {
        LogOnce("CreateMaterial: only materials of the unlit and lit built-in shaders exist so far (text: stage E of #43); "
                "returning null");
        return nullptr;
    }

    DrawState* const draw = _draw.get(); // the materials die with it, so it outlives them
    auto material = std::make_unique<VulkanMaterial>(
        chosen->kind, shader, texture, [this](const std::string& message) { LogOnce(message); },
        [draw](const ITexture* candidate) -> std::uint64_t
        {
            std::lock_guard idLock(draw->textureIdMutex);
            const auto found = draw->textureIds.find(candidate);
            return found != draw->textureIds.end() ? found->second : 0;
        });
    IMaterial* raw = material.get();
    _draw->materials.emplace(raw, std::move(material));
    return raw;
}

void VulkanRenderer::DestroyMaterial(IMaterial* material)
{
    if (material == nullptr)
    {
        return;
    }
    std::lock_guard lock(_mutex);
    // A draw copied what it needs into the ring when it was recorded, so a material can go at once, even in a frame that
    // used it. Foreign pointers are not keys and are ignored
    _draw->materials.erase(material);
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------------------------------

void VulkanRenderer::SetViewProjection(const float* view, const float* projection)
{
    std::lock_guard lock(_mutex);
    if (view != nullptr)
    {
        std::memcpy(_draw->view, view, sizeof(_draw->view));
    }
    if (projection != nullptr)
    {
        std::memcpy(_draw->projection, projection, sizeof(_draw->projection));
    }
}

void VulkanRenderer::UpdateSceneLighting(const Common::SceneLightingData& lighting,
                                         const N2Engine::Math::Vector3& cameraPosition)
{
    // Builds the block now (a pure function of its arguments) under the lock and stores it: no GPU memory is touched. The
    // lit draws made after this call copy it into the ring (see the header comment: GL semantics, and nothing a frame in
    // flight reads is rewritten). Called inside a frame, outside one, or several times in a frame; before the first call the
    // block holds the defaults
    const LightingBlock block = BuildLightingBlock(lighting, cameraPosition);
    std::lock_guard lock(_mutex);
    _draw->lighting = block;
    ++_draw->lightingVersion;
}

bool VulkanRenderer::GetMaterialBlockForTest(const IMaterial* material, std::vector<std::uint8_t>& bytes) const
{
    std::lock_guard lock(_mutex);
    const auto it = _draw->materials.find(material);
    if (it == _draw->materials.end())
    {
        return false;
    }
    const VulkanMaterial::Snapshot snapshot = it->second->TakeSnapshot();
    bytes.assign(snapshot.block.begin(), snapshot.block.end());
    return true;
}

std::vector<std::uint8_t> VulkanRenderer::GetLightingBlockForTest() const
{
    std::lock_guard lock(_mutex);
    return std::vector<std::uint8_t>(_draw->lighting.begin(), _draw->lighting.end());
}

void VulkanRenderer::SetWireframe(const bool enabled)
{
    std::lock_guard lock(_mutex);
    // Before Initialize the device's features are not known yet: the request is kept and Initialize checks it
    if (enabled && _initialized && !_draw->wireframeSupported)
    {
        LogOnce(kWireframeIgnoredMessage);
        _draw->wireframe = false;
        return;
    }
    _draw->wireframe = enabled;
}

void VulkanRenderer::DrawMesh(IMesh* mesh, const float* modelMatrix, IMaterial* material, const RenderState& state)
{
    std::lock_guard lock(_mutex);
    DrawLocked(mesh, modelMatrix, material, state, 0, 0, true);
}

void VulkanRenderer::DrawMesh(IMesh* mesh, const float* modelMatrix, IMaterial* material, const RenderState& state,
                              const Common::IndexRange& range)
{
    std::lock_guard lock(_mutex);
    DrawLocked(mesh, modelMatrix, material, state, range.first, range.count, false);
}

void VulkanRenderer::DrawObjects(const std::vector<Common::RenderObject>& objects)
{
    std::lock_guard lock(_mutex);
    for (const Common::RenderObject& object : objects)
    {
        DrawLocked(object.mesh, object.transform.model, object.material, object.state, 0, 0, true);
    }
}

void VulkanRenderer::DrawLocked(IMesh* mesh, const float* modelMatrix, IMaterial* material, const RenderState& state,
                                uint32_t first, uint32_t count, const bool wholeMesh)
{
    if (!_initialized || _deviceLost || !_frameOpen || mesh == nullptr || modelMatrix == nullptr || material == nullptr)
    {
        return;
    }
    DrawState& draw = *_draw;

    // Foreign (or destroyed) pointers are only looked up in the registries, never dereferenced
    const auto meshIt = _resources->meshes.find(mesh);
    if (meshIt == _resources->meshes.end())
    {
        return;
    }
    // Read under the lock: UpdateMesh swaps the buffers. A mesh with vertices and no indices has no index buffer and
    // draws nothing (as OpenGL: DrawIndices returns for an index count of 0)
    const VulkanMesh& vulkanMesh = *meshIt->second;
    if (vulkanMesh.vertexBuffer == VK_NULL_HANDLE || vulkanMesh.indexBuffer == VK_NULL_HANDLE || vulkanMesh.indexCount == 0)
    {
        return;
    }
    if (wholeMesh)
    {
        first = 0;
        count = vulkanMesh.indexCount;
    }
    else if (count == 0 || static_cast<std::uint64_t>(first) + count > vulkanMesh.indexCount)
    {
        return; // GL semantics: a range that is not inside the index buffer draws nothing
    }

    const auto materialIt = draw.materials.find(material);
    if (materialIt == draw.materials.end())
    {
        return;
    }
    const VulkanMaterial& vulkanMaterial = *materialIt->second;

    const VkPipeline pipeline = GetPipelineLocked(vulkanMaterial.kind, state, draw.wireframe);
    if (pipeline == VK_NULL_HANDLE)
    {
        return; // no shaders in this build, or the pipeline could not be made (logged)
    }

    // The textures: a texture the renderer no longer has (destroyed since it was set) is replaced by the 1x1 default
    const VulkanMaterial::Snapshot snapshot = vulkanMaterial.TakeSnapshot();
    std::array<VkImageView, kSamplerCount> views{};
    std::array<VkSampler, kSamplerCount> samplers{};
    for (std::size_t i = 0; i < kSamplerCount; ++i)
    {
        const VulkanTexture* texture = nullptr;
        if (snapshot.textures[i] != nullptr)
        {
            // Registered AND the same texture that was set: an id from the counter never repeats, so a texture made at the
            // address of a destroyed one is not taken for it (the slot gets the default, like any destroyed texture)
            if (const auto it = _resources->textures.find(snapshot.textures[i]);
                it != _resources->textures.end() && snapshot.textureIds[i] != 0 &&
                TextureIdLocked(snapshot.textures[i]) == snapshot.textureIds[i])
            {
                texture = it->second.get();
            }
        }
        if (texture == nullptr || texture->view == VK_NULL_HANDLE || texture->sampler == VK_NULL_HANDLE)
        {
            texture = draw.defaults[i].get();
        }
        if (texture == nullptr)
        {
            return;
        }
        views[i] = texture->view;
        samplers[i] = texture->sampler;
    }

    // This draw's room in the ring: the per-draw block, then the material block, each at an aligned offset; and, for a lit
    // draw whose chunk does not hold the current lighting yet, the lighting block after them
    const uint32_t slot = _currentFrame;
    FrameDrawState& frame = draw.frames[slot];
    const bool lit = vulkanMaterial.kind == BuiltInShader::Lit;
    const VkDeviceSize perDrawBytes = AlignUp(kPerDrawBlockSize, draw.uniformAlignment);
    const VkDeviceSize materialBytes = AlignUp(kMaterialBlockRange, draw.uniformAlignment);
    const VkDeviceSize lightingBytes = AlignUp(kLightingBlockSize, draw.uniformAlignment);
    std::size_t chunkIndex = 0;
    VkDeviceSize offset = 0;
    bool copyLighting = false;
    for (;;)
    {
        // Ask for the lighting too when the chunk the draw would start in does not hold this version of it. If the chunk is
        // full the allocation moves to the next, which holds none: then ask again with the lighting included
        copyLighting = lit &&
            !(frame.activeChunk < frame.chunks.size() && frame.chunks[frame.activeChunk].lightingVersion == draw.lightingVersion);
        if (!AllocateRing(slot, perDrawBytes + materialBytes + (copyLighting ? lightingBytes : 0), chunkIndex, offset))
        {
            return;
        }
        if (!lit || copyLighting || frame.chunks[chunkIndex].lightingVersion == draw.lightingVersion)
        {
            break;
        }
    }
    RingChunk& chunk = frame.chunks[chunkIndex];

    // Row-major matrices, copied as the engine passes them (the GLSL block is row_major, the counterpart of GL's
    // transpose = GL_TRUE upload)
    std::uint8_t* perDraw = chunk.mapped + offset;
    std::memcpy(perDraw, modelMatrix, 64);
    std::memcpy(perDraw + 64, draw.view, 64);
    std::memcpy(perDraw + 128, draw.projection, 64);
    std::memcpy(chunk.mapped + offset + perDrawBytes, snapshot.block.data(), snapshot.block.size());
    if (copyLighting)
    {
        // The first lit draw of this lighting version in this chunk: its copy goes after the material block
        chunk.lightingOffset = offset + perDrawBytes + materialBytes;
        chunk.lightingVersion = draw.lightingVersion;
        std::memcpy(chunk.mapped + chunk.lightingOffset, draw.lighting.data(), draw.lighting.size());
    }

    // Set 0: this chunk's per-draw range and the lighting buffer, made once per chunk per frame
    if (chunk.frameSet == VK_NULL_HANDLE)
    {
        const VkDescriptorSet set = AllocateSet(slot, draw.frameSetLayout);
        if (set == VK_NULL_HANDLE)
        {
            return;
        }
        VkDescriptorBufferInfo perDrawInfo{chunk.buffer, 0, kPerDrawBlockSize};
        VkDescriptorBufferInfo lightingInfo{chunk.buffer, 0, kLightingBlockSize}; // the range at the draw's dynamic offset
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[0].pBufferInfo = &perDrawInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[1].pBufferInfo = &lightingInfo;
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        chunk.frameSet = set;
    }

    // Set 1: this chunk's material range and the five textures. A set holds no per-draw data (the offsets are dynamic), so
    // draws with the same textures in the same chunk share one
    std::array<std::uint64_t, 11> key{};
    key[0] = chunkIndex;
    for (std::size_t i = 0; i < kSamplerCount; ++i)
    {
        key[1 + i] = HandleValue(views[i]);
        key[1 + kSamplerCount + i] = HandleValue(samplers[i]);
    }
    VkDescriptorSet materialSet = VK_NULL_HANDLE;
    if (const auto it = frame.materialSets.find(key); it != frame.materialSets.end())
    {
        materialSet = it->second;
    }
    else
    {
        materialSet = AllocateSet(slot, draw.materialSetLayout);
        if (materialSet == VK_NULL_HANDLE)
        {
            return;
        }
        VkDescriptorBufferInfo materialInfo{chunk.buffer, 0, kMaterialBlockRange};
        std::array<VkDescriptorImageInfo, kSamplerCount> imageInfos{};
        std::array<VkWriteDescriptorSet, 1 + kSamplerCount> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = materialSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[0].pBufferInfo = &materialInfo;
        for (std::uint32_t i = 0; i < kSamplerCount; ++i)
        {
            imageInfos[i].sampler = samplers[i];
            imageInfos[i].imageView = views[i];
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1 + i].dstSet = materialSet;
            writes[1 + i].dstBinding = kFirstSamplerBinding + i;
            writes[1 + i].descriptorCount = 1;
            writes[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1 + i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        frame.materialSets.emplace(key, materialSet);
    }

    const VkCommandBuffer commandBuffer = _frames[slot].commandBuffer;
    const VkDescriptorSet sets[2] = {chunk.frameSet, materialSet};
    // In the order of the dynamic descriptors: set 0 binding 0 (per-draw), set 0 binding 1 (lighting), set 1 binding 0
    // (material). A draw that does not read the lighting (unlit) names the chunk's copy if it has one, else offset 0, which
    // is in range (the chunks are far bigger than the block) and is never read
    const uint32_t dynamicOffsets[3] = {static_cast<uint32_t>(offset), static_cast<uint32_t>(chunk.lightingOffset),
                                        static_cast<uint32_t>(offset + perDrawBytes)};
    const VkDeviceSize vertexOffset = 0;
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipelineLayout, 0, 2, sets, 3,
                            dynamicOffsets);
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vulkanMesh.vertexBuffer, &vertexOffset);
    vkCmdBindIndexBuffer(commandBuffer, vulkanMesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(commandBuffer, count, 1, first, 0, 0);
}
