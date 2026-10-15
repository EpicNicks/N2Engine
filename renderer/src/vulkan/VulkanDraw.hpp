#pragma once

// PRIVATE to renderer/src/vulkan/: what stage B of #43 adds to the Vulkan renderer (shaders, materials, the pipeline
// layout and cache, the per-draw uniform ring and the descriptor pools), behind one pointer so the public header names
// none of it. Includes VMA through VulkanResources.hpp, so it must never be included from a public header.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "VulkanResources.hpp"

#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/vulkan/VulkanShaderInterface.hpp"

namespace Renderer::Vulkan
{
    /// One of the three built-in shaders: a vertex and a fragment module made from the SPIR-V compiled at build time.
    /// There is no custom GLSL in v1, so LoadFromStrings fails and the Set* calls (a shader holds no values here, the
    /// material does) do nothing.
    class VulkanShader final : public Common::IShader
    {
    public:
        explicit VulkanShader(const ShaderInterface::BuiltInShader shaderKind) : kind(shaderKind) {}

        bool LoadFromStrings(const std::string&, const std::string&) override { return false; }
        void Bind() const override {}
        void Unbind() const override {}
        bool IsValid() const override { return vertexModule != VK_NULL_HANDLE && fragmentModule != VK_NULL_HANDLE; }

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
        VkShaderModule vertexModule = VK_NULL_HANDLE;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
    };

    /// A material of a built-in shader: its values live in a CPU copy of the shader's std140 material block, written
    /// through the name table (VulkanShaderInterface.hpp), plus the textures. DrawMesh snapshots both, so a change
    /// after a draw does not affect it. Thread-safe on its own (a material may be set from any thread).
    class VulkanMaterial final : public Common::IMaterial
    {
    public:
        using Warn = std::function<void(const std::string&)>;

        VulkanMaterial(ShaderInterface::BuiltInShader shaderKind, Common::IShader* owner, Common::ITexture* baseTexture,
                       Warn warn);

        void SetInt(const std::string& name, int value) override;
        void SetFloat(const std::string& name, float value) override;
        void SetVec2(const std::string& name, float x, float y) override;
        void SetVec2(const std::string& name, N2Engine::Math::Vector2& value) override;
        void SetVec3(const std::string& name, float x, float y, float z) override;
        void SetVec3(const std::string& name, N2Engine::Math::Vector3& value) override;
        void SetVec4(const std::string& name, float x, float y, float z, float w) override;
        void SetVec4(const std::string& name, N2Engine::Math::Vector4& value) override;
        void SetColor(const std::string& name, float r, float g, float b, float a) override;
        void SetTexture(Common::ITexture* texture) override;
        void SetAuxTexture(Common::AuxTexture which, Common::ITexture* texture) override;
        [[nodiscard]] Common::ITexture* GetAuxTexture(Common::AuxTexture which) const override;

        [[nodiscard]] Common::IShader* GetShader() const override { return shader; }
        [[nodiscard]] Common::ITexture* GetTexture() const override;
        [[nodiscard]] bool IsValid() const override { return shader != nullptr; }

        /// What a draw takes: the block bytes and the five textures (base, then AuxTexture order), under the lock.
        /// The texture pointers are only compared by the caller, never dereferenced
        struct Snapshot
        {
            std::array<std::uint8_t, ShaderInterface::kMaterialBlockRange> block{};
            std::array<const Common::ITexture*, ShaderInterface::kSamplerCount> textures{};
        };
        [[nodiscard]] Snapshot TakeSnapshot() const;

        const ShaderInterface::BuiltInShader kind;

    private:
        /// The uniform called `name` if it exists in the material block with the type `expected`; else logs (once per
        /// name) and returns nullptr. Call with the lock held
        const ShaderInterface::Uniform* Find(const std::string& name, ShaderInterface::UniformType expected,
                                             const char* setter) const;
        void WriteFloats(std::uint32_t offset, const float* values, std::uint32_t count);

        mutable std::mutex _mutex;
        Common::IShader* shader;
        Warn _warn;
        std::array<std::uint8_t, ShaderInterface::kMaterialBlockRange> _block{};
        Common::ITexture* _texture = nullptr;
        std::array<Common::ITexture*, 4> _aux{};
    };

    /// A piece of the per-draw uniform ring: a host-visible, persistently mapped uniform buffer. The draws of a frame
    /// fill it from the start; when it is full the frame chains another (bigger) one, so any number of draws works.
    struct RingChunk
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
        std::uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
        VkDescriptorSet frameSet = VK_NULL_HANDLE; // set 0 for this chunk, from this frame's pool; null until needed
    };

    /// Everything one frame slot owns that only its own frame (and the one before it has finished) may touch
    struct FrameDrawState
    {
        std::vector<RingChunk> chunks;
        std::size_t activeChunk = 0;
        std::vector<VkDescriptorPool> pools;
        std::size_t activePool = 0;
        std::uint32_t activePoolSets = 0;
        // The material sets made so far this frame, by (chunk, the five views, the five samplers): a set holds only the
        // chunk's buffer and the textures (the per-draw offsets are dynamic), so draws with the same textures share one
        std::map<std::array<std::uint64_t, 11>, VkDescriptorSet> materialSets;
    };

    /// The pipeline cache key: (shader, RenderState, wireframe) packed
    struct PipelineKey
    {
        std::uint8_t shader = 0;
        bool depthTest = true;
        bool depthWrite = true;
        std::uint8_t cull = 0;
        bool blend = false;
        bool additive = false;
        bool wireframe = false;

        [[nodiscard]] std::uint32_t Pack() const
        {
            return static_cast<std::uint32_t>(shader) | (depthTest ? 1u << 8 : 0u) | (depthWrite ? 1u << 9 : 0u) |
                (static_cast<std::uint32_t>(cull) << 10) | (blend ? 1u << 12 : 0u) | (additive ? 1u << 13 : 0u) |
                (wireframe ? 1u << 14 : 0u);
        }
    };

    struct DrawState
    {
        // Layout: set 0 the frame (per-draw dynamic UBO, lighting UBO), set 1 the material (dynamic UBO + 5 samplers)
        VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;

        // The three built-ins. The modules exist only when the SPIR-V was built (N2ENGINE_VULKAN_HAS_SPIRV)
        std::array<std::unique_ptr<VulkanShader>, ShaderInterface::kBuiltInShaderCount> shaders;

        std::unordered_map<const Common::IMaterial*, std::unique_ptr<VulkanMaterial>> materials;
        std::map<std::uint32_t, VkPipeline> pipelines;

        // 1x1 textures for the absent ones (base, emissive, occlusion, normal, metallic-roughness)
        std::array<std::unique_ptr<VulkanTexture>, ShaderInterface::kSamplerCount> defaults;

        // The lighting block: zero-filled in stage B (stage C fills it), so lit and text pipelines are layout-complete
        VkBuffer lightingBuffer = VK_NULL_HANDLE;
        VmaAllocation lightingAllocation = nullptr;

        std::array<FrameDrawState, 2> frames; // one per frame slot (VulkanRenderer::MAX_FRAMES_IN_FLIGHT)

        VkDeviceSize uniformAlignment = 256; // minUniformBufferOffsetAlignment
        // VMA allocations that are the renderer's own (rings, lighting buffer, default textures): the test hook that
        // counts a caller's allocations leaves them out
        std::uint32_t internalAllocations = 0;

        float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        bool wireframe = false;
        bool wireframeSupported = false;
    };
}
