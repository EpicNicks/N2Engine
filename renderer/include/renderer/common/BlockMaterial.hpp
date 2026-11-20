#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/common/GpuShaderInterface.hpp"
#include "renderer/common/IMaterial.hpp"
#include "renderer/common/IShader.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/common/UserShader.hpp"

// The backend-neutral part of a GPU backend's material (#43, stage B; moved here from the Vulkan backend's private
// VulkanDraw.hpp, unchanged): the values of a built-in shader's std140 material block and its textures, with no GPU API
// in it. The Vulkan backend uses it as it is (it has nothing Vulkan-specific to add: it snapshots the block and the
// textures at each draw).
namespace Renderer::Gpu
{
    using ShaderInterface::BuiltInShader;

    /// The most bytes any material block takes (a user shader's, kUserShaderMaxBlockBytes). A built-in shader's is
    /// ShaderInterface::kMaterialBlockRange (128); a user shader's descriptor range is its own (MaterialLayout::range), so the
    /// built-ins' per-draw ring use is what it always was
    inline constexpr std::uint32_t kMaxMaterialBlockBytes = Common::kUserShaderMaxBlockBytes;
    static_assert(kMaxMaterialBlockBytes >= ShaderInterface::kMaterialBlockRange);

    /// The material block of a USER shader as a layout provider (proposal section 6.1): the member table built from the
    /// cooked manifest, the values a fresh material holds, and what an unset texture slot reads. Immutable and shared by the
    /// shader and every material made from it, so a material outlives the shader's destruction safely. Never copied or
    /// moved: the uniform names are views into `names`
    struct MaterialLayout
    {
        MaterialLayout() = default;
        MaterialLayout(const MaterialLayout&) = delete;
        MaterialLayout& operator=(const MaterialLayout&) = delete;

        /// What a new material holds for one member
        struct Default
        {
            std::size_t uniform;                ///< index into `uniforms`
            std::array<float, 4> values;
        };

        std::string shaderName;
        std::vector<std::string> names;         ///< owns the characters the uniform names view
        std::vector<ShaderInterface::Uniform> uniforms;
        std::vector<Default> defaults;
        std::uint32_t blockSize = 0;            ///< the generated block's bytes
        std::uint32_t range = 0;                ///< what a material descriptor covers: the block rounded up to 16 (at most 256)
        std::array<Common::UserTextureDefault, ShaderInterface::kSamplerCount> textureDefaults{};
        std::array<bool, ShaderInterface::kSamplerCount> textureDeclared{}; ///< the slots the manifest declares a texture for

        /// The layout of an accepted blob (ValidateUserShaderBlob returned empty); null for a blob it rejects
        [[nodiscard]] static std::shared_ptr<const MaterialLayout> FromUserShader(const Common::UserShaderBlob& blob);
    };

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
        /// A material of a user shader: the block follows `layout` (kind is Unlit and means nothing; IsUserShader is true)
        BlockMaterial(std::shared_ptr<const MaterialLayout> layout, Common::IShader* owner, Common::ITexture* baseTexture,
                      Warn warn, TextureIdOf textureIdOf);

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
        [[nodiscard]] bool IsUserShaderMaterial() const override { return IsUserShader(); }

        /// What a draw takes: the block bytes (`blockBytes` of them are the material's range: 128 for a built-in shader,
        /// the layout's for a user shader; the rest of the array is zero) and the five textures (base, then AuxTexture
        /// order), under the lock. The texture pointers are only compared by the caller, never dereferenced; textureIds
        /// holds the id each texture had when it was set (0: none, or not a live texture then)
        struct Snapshot
        {
            std::array<std::uint8_t, kMaxMaterialBlockBytes> block{};
            std::uint32_t blockBytes = ShaderInterface::kMaterialBlockRange;
            std::array<const Common::ITexture*, ShaderInterface::kSamplerCount> textures{};
            std::array<std::uint64_t, ShaderInterface::kSamplerCount> textureIds{};
        };
        [[nodiscard]] Snapshot TakeSnapshot() const;

        /// Test hook: replaces the id a texture slot (0 the base texture, 1-4 the AuxTexture slots) holds, so a test can
        /// put a stale (pointer, id) pair into a material, as a texture made at the address of a destroyed one would
        void SetTextureIdForTest(std::size_t slot, std::uint64_t id);

        /// The built-in shader this material is of. Meaningless (Unlit) when IsUserShader
        const BuiltInShader kind;
        [[nodiscard]] bool IsUserShader() const { return _layout != nullptr; }
        [[nodiscard]] const MaterialLayout* UserLayout() const { return _layout.get(); }
        [[nodiscard]] Common::UserTextureDefault TextureDefault(std::size_t slot) const
        {
            return (_layout && slot < _layout->textureDefaults.size()) ? _layout->textureDefaults[slot]
                                                                        : Common::UserTextureDefault::White;
        }

    private:
        /// The uniform called `name` if it exists in the material block with the type `expected`; else logs (once per
        /// name) and returns nullptr. Call with the lock held
        const ShaderInterface::Uniform* Find(const std::string& name, ShaderInterface::UniformType expected,
                                             const char* setter) const;
        void WriteFloats(std::uint32_t offset, const float* values, std::uint32_t count);
        [[nodiscard]] std::uint32_t BlockRange() const
        {
            return _layout ? _layout->range : ShaderInterface::kMaterialBlockRange;
        }

        mutable std::mutex _mutex;
        Common::IShader* shader;
        Warn _warn;
        TextureIdOf _textureIdOf;
        std::shared_ptr<const MaterialLayout> _layout; // null: a built-in shader's material
        std::array<std::uint8_t, kMaxMaterialBlockBytes> _block{};
        Common::ITexture* _texture = nullptr;
        std::uint64_t _textureId = 0;
        std::array<Common::ITexture*, 4> _aux{};
        std::array<std::uint64_t, 4> _auxIds{};
    };
}
