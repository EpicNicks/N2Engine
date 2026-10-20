#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/common/GpuShaderInterface.hpp"
#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/ITexture.hpp"

// The backend-neutral part of a GPU backend's material (#43, stage B; moved here from the Vulkan backend's private
// VulkanDraw.hpp, unchanged): the values of a built-in shader's std140 material block and its textures, with no GPU API
// in it. The Vulkan backend uses it as it is (it has nothing Vulkan-specific to add: it snapshots the block and the
// textures at each draw).
namespace Renderer::Gpu
{
    using ShaderInterface::BuiltInShader;

    /// A material of a built-in shader: its values live in a CPU copy of the shader's std140 material block, written
    /// through the name table (GpuShaderInterface.hpp), plus the textures. DrawMesh snapshots both, so a change
    /// after a draw does not affect it. Thread-safe on its own (a material may be set from any thread).
    class BlockMaterial final : public Common::IMaterial
    {
    public:
        using Warn = std::function<void(const std::string&)>;
        /// The renderer's id for a texture (0 for a pointer that is not a live texture of the renderer). Taken when a
        /// texture is set, so a later texture made at the same address is told apart from the one that was set. It takes
        /// only a leaf lock, never the renderer's, so it may be called from any thread
        using TextureIdOf = std::function<std::uint64_t(const Common::ITexture*)>;

        BlockMaterial(BuiltInShader shaderKind, Common::IShader* owner, Common::ITexture* baseTexture, Warn warn,
                      TextureIdOf textureIdOf);

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
        /// The texture pointers are only compared by the caller, never dereferenced; textureIds holds the id each
        /// texture had when it was set (0: none, or not a live texture then)
        struct Snapshot
        {
            std::array<std::uint8_t, ShaderInterface::kMaterialBlockRange> block{};
            std::array<const Common::ITexture*, ShaderInterface::kSamplerCount> textures{};
            std::array<std::uint64_t, ShaderInterface::kSamplerCount> textureIds{};
        };
        [[nodiscard]] Snapshot TakeSnapshot() const;

        /// Test hook: replaces the id a texture slot (0 the base texture, 1-4 the AuxTexture slots) holds, so a test can
        /// put a stale (pointer, id) pair into a material, as a texture made at the address of a destroyed one would
        void SetTextureIdForTest(std::size_t slot, std::uint64_t id);

        const BuiltInShader kind;

    private:
        /// The uniform called `name` if it exists in the material block with the type `expected`; else logs (once per
        /// name) and returns nullptr. Call with the lock held
        const ShaderInterface::Uniform* Find(const std::string& name, ShaderInterface::UniformType expected,
                                             const char* setter) const;
        void WriteFloats(std::uint32_t offset, const float* values, std::uint32_t count);

        mutable std::mutex _mutex;
        Common::IShader* shader;
        Warn _warn;
        TextureIdOf _textureIdOf;
        std::array<std::uint8_t, ShaderInterface::kMaterialBlockRange> _block{};
        Common::ITexture* _texture = nullptr;
        std::uint64_t _textureId = 0;
        std::array<Common::ITexture*, 4> _aux{};
        std::array<std::uint64_t, 4> _auxIds{};
    };
}
