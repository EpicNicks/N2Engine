#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// What a backend is given to make a user shader (orchestrator/proposals/user-shaders.md, stage 2): the cooked result of
// tools/n2shader for one target, with no GPU API and no file format in it. The cook's manifest.json and the payload
// bytes are turned into this by the engine (stage 5) or, in the renderer's tests, by tests/renderer/common/UserShaderFixture.hpp.
//
// The blob is UNTRUSTED input (a pack or a mod may carry a hostile one): ValidateUserShaderBlob is what a backend calls
// before it touches a driver, and every limit of the proposal's section 8 is checked there.
namespace Renderer::Common
{
    /// The types of a declared parameter (proposal section 3.2). Every one but Texture is a member of the std140 block
    enum class UserParamType : std::uint8_t
    {
        Float,
        Int,
        Bool,    ///< an int in the block (0 or 1)
        Vec2,
        Vec3,
        Vec4,
        Color,   ///< a vec4 with an editor colour picker
        Texture, ///< one of the five 2D texture slots; not in the block
    };

    /// What an unset texture slot reads (the manifest's texture "default")
    enum class UserTextureDefault : std::uint8_t
    {
        White,
        Black,
        Normal, ///< the flat tangent-space normal (0.5, 0.5, 1)
        None,   ///< transparent black (0, 0, 0, 0)
    };

    /// One declared parameter, as the cook's manifest lists it
    struct UserParam
    {
        std::string name;
        UserParamType type = UserParamType::Float;
        std::uint32_t offset = 0; ///< std140 offset in the material block (not for a texture)
        std::uint32_t size = 0;   ///< bytes the member occupies (not for a texture)
        std::uint32_t slot = 0;   ///< the texture slot 0-4 (a texture only)
        std::array<float, 4> defaultValue{}; ///< the components of a non-texture default (a bool or int: the value cast)
        UserTextureDefault textureDefault = UserTextureDefault::White;
    };

    /// The cooked payload for one backend plus what the renderer needs to lay out the material and the pipeline
    struct UserShaderBlob
    {
        std::string name;       ///< the shader's display name, for log lines
        std::string sourcePath; ///< the .n2shader path, for log lines (may be empty)
        std::uint32_t contract = 1;
        std::uint32_t blockSize = 0; ///< bytes of the generated material block (std140)
        bool usesLighting = false;   ///< "uses": ["lighting"]: the draw copies the scene lighting for this shader
        std::vector<UserParam> params;
        /// The Vulkan fragment stage: SPIR-V words (shader.vulkan.frag.spv, as little-endian words)
        std::vector<std::uint32_t> fragmentSpirv;
    };

    /// The limits the checks below enforce (proposal section 8). Exposed so tests and tools quote one source
    inline constexpr std::uint32_t kUserShaderMaxBlockBytes = 256;
    inline constexpr std::uint32_t kUserShaderMaxParams = 64;
    inline constexpr std::uint32_t kUserShaderTextureSlots = 5;
    inline constexpr std::size_t kUserShaderMaxSpirvBytes = 256 * 1024;
    inline constexpr std::size_t kUserShaderMaxNameBytes = 64;
    inline constexpr std::uint32_t kUserShaderContractVersion = 1;
    /// How many user shaders a renderer keeps at once (a hostile pack cannot grow them without bound)
    inline constexpr std::size_t kMaxUserShaders = 256;
    /// How many user-shader pipelines a renderer keeps at once; past it the least recently used are evicted
    inline constexpr std::size_t kMaxUserPipelines = 4096;

    /// The varyings of the engine's vertex stage (contract v1), one bit per fragment input location 0-4. The Vulkan backend
    /// pairs a fragment stage with the vertex variant that writes exactly the varyings the fragment reads (see
    /// renderer/shaders/vulkan/user.vert)
    inline constexpr std::uint32_t kUserVaryingUv = 1u << 0;
    inline constexpr std::uint32_t kUserVaryingColor = 1u << 1;
    inline constexpr std::uint32_t kUserVaryingWorldPos = 1u << 2;
    inline constexpr std::uint32_t kUserVaryingNormal = 1u << 3;
    inline constexpr std::uint32_t kUserVaryingTangent = 1u << 4;
    inline constexpr std::uint32_t kUserVaryingCount = 5;
    inline constexpr std::uint32_t kUserVaryingMasks = 1u << kUserVaryingCount; // the number of vertex variants

    /// The vertex attribute locations (GpuShaderInterface.hpp: position 0, normal 1, texcoord 2, colour 3, tangent 4) the
    /// vertex variant for `varyingMask` reads, one bit per location. Position is always read
    [[nodiscard]] constexpr std::uint32_t UserVertexAttributeMask(const std::uint32_t varyingMask)
    {
        return (1u << 0) | ((varyingMask & kUserVaryingNormal) ? 1u << 1 : 0u) | ((varyingMask & kUserVaryingUv) ? 1u << 2 : 0u) |
            ((varyingMask & kUserVaryingColor) ? 1u << 3 : 0u) | ((varyingMask & kUserVaryingTangent) ? 1u << 4 : 0u);
    }

    /// Reads which fragment inputs (locations 0-4, as a mask of the kUserVarying bits) a SPIR-V fragment module declares in
    /// its entry point's interface, walking the module's instructions with every length checked. False, with `error` set,
    /// for a module that is truncated, has no single fragment entry point, reads an input location past 4, or declares a
    /// capability outside the contract's list (Shader, Matrix, ImageQuery, DerivativeControl, Sampled1D, SampledBuffer,
    /// Float16, Int16, Int8): the allow-list of the proposal's section 8, applied before a driver sees the module
    [[nodiscard]] bool ReadFragmentVaryings(const std::vector<std::uint32_t>& spirv, std::uint32_t& mask, std::string& error);

    /// The reason `blob` is not acceptable, or an empty string when it is: the contract version, the size limits, parameter
    /// names (unique, identifiers, not starting with n2_ or gl_), offsets (inside the block, naturally aligned, the std140
    /// alignment of the type, no overlaps), texture slots (0-4, unique), and the SPIR-V (a non-empty word array of at most
    /// kUserShaderMaxSpirvBytes starting with the SPIR-V magic number, and passing ReadFragmentVaryings)
    [[nodiscard]] std::string ValidateUserShaderBlob(const UserShaderBlob& blob);
}
