#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "renderer/common/RenderTypes.hpp"

// The interface of the Vulkan backend's three built-in shaders (#43, stage B), as C++: which uniform lives in which
// block, at which std140 offset, and where the vertex attributes and samplers are. There is no SPIR-V reflection: this
// table IS the contract between the C++ side (VulkanMaterial writes a name's value at its offset; the pipeline layout and
// vertex input follow it) and the GLSL under renderer/shaders/vulkan/. tests/renderer/vulkan/VulkanShaderParityTests.cpp
// parses the GLSL as text and checks every entry against the declaration (block, member order, type, std140 offset),
// every uniform Material::ApplyUniforms sets against it, and the vertex attributes against Common::Vertex.
//
// Vulkan-free on purpose (no vulkan.h), so the parity test and anything else can include it on any machine.
//
// Blocks (std140):
//   set 0, binding 0  PerDraw   (dynamic offset per draw)  uModel, uView, uProjection: row-major matrices, copied as the
//                                                          engine gives them (the GLSL says row_major), 192 bytes
//   set 0, binding 1  Lighting  (dynamic offset per draw)  scene lighting for the lit shader (stage C: UpdateSceneLighting
//                                                          keeps a CPU copy; the first lit draw after each update copies
//                                                          it into the frame's ring, see VulkanDraw.cpp)
//   set 1, binding 0  Material  (dynamic offset per draw)  the material's values, snapshotted at DrawMesh
//   set 1, binding 1..5 samplers                           the base texture, then the AuxTexture slots in enum order
namespace Renderer::Vulkan::ShaderInterface
{
    enum class BuiltInShader : std::uint8_t
    {
        Unlit,
        Lit,
        Text,
    };

    inline constexpr std::size_t kBuiltInShaderCount = 3;

    enum class Block : std::uint8_t
    {
        PerDraw,
        Lighting,
        Material,
    };

    enum class UniformType : std::uint8_t
    {
        Int,    // int (a GLSL bool is an int in a block: 0 or 1)
        Float,
        Vec2,
        Vec3,
        Vec4,
        Mat4,
        Struct, // an array of light structs (the lighting block): checked by offset and size only
    };

    /// One member of a block. `size` is the bytes the member occupies (an array: all of it); `arrayCount` 1 for a
    /// plain member
    struct Uniform
    {
        std::string_view name;
        Block block;
        std::uint32_t offset;
        UniformType type;
        std::uint32_t size;
        std::uint32_t arrayCount;
    };

    /// One vertex attribute a shader reads: its `layout(location = N)`, its float count, and its byte offset in
    /// Common::Vertex
    struct Attribute
    {
        std::string_view name;
        std::uint32_t location;
        std::uint32_t components;
        std::uint32_t offset;
    };

    struct Sampler
    {
        std::string_view name;
        std::uint32_t binding;
    };

    // ---- Fixed sizes -------------------------------------------------------------------------------------------------
    inline constexpr std::uint32_t kPerDrawBlockSize = 192;   // 3 mat4
    /// The range every material descriptor covers: at least the largest material block (the lit one is 72 bytes, the
    /// text one 92), so one descriptor set layout serves every shader
    inline constexpr std::uint32_t kMaterialBlockRange = 128;
    inline constexpr std::uint32_t kLightingBlockSize = 752;  // see the Lighting table
    inline constexpr std::uint32_t kSamplerCount = 5;         // base + 4 AuxTexture slots
    inline constexpr std::uint32_t kFirstSamplerBinding = 1;  // set 1, binding 0 is the material block
    inline constexpr std::uint32_t kDirectionalLightCount = 2;
    inline constexpr std::uint32_t kPointLightCount = 8;
    inline constexpr std::uint32_t kSpotLightCount = 4;

    // ---- Attributes: where Common::Vertex's fields are --------------------------------------------------------------
    inline constexpr Attribute kPosition{"aPos", 0, 3, 0};
    inline constexpr Attribute kNormal{"aNormal", 1, 3, 12};
    inline constexpr Attribute kTexCoord{"aTexCoord", 2, 2, 24};
    inline constexpr Attribute kColor{"aColor", 3, 4, 32};
    inline constexpr Attribute kTangent{"aTangent", 4, 4, 48};

    static_assert(kPosition.offset == offsetof(Common::Vertex, position));
    static_assert(kNormal.offset == offsetof(Common::Vertex, normal));
    static_assert(kTexCoord.offset == offsetof(Common::Vertex, texCoord));
    static_assert(kColor.offset == offsetof(Common::Vertex, color));
    static_assert(kTangent.offset == offsetof(Common::Vertex, tangent));
    static_assert(sizeof(Common::Vertex) == 64);

    /// Unlit and text read position, texture coordinates and colour (the layer warns about an attribute nothing
    /// consumes, so the pipeline provides exactly what the shader reads); lit reads all five
    inline constexpr std::array<Attribute, 3> kUnlitAttributes{kPosition, kTexCoord, kColor};
    inline constexpr std::array<Attribute, 5> kLitAttributes{kPosition, kNormal, kTexCoord, kColor, kTangent};
    inline constexpr std::array<Attribute, 3> kTextAttributes{kPosition, kTexCoord, kColor};

    // ---- Uniforms ---------------------------------------------------------------------------------------------------
    inline constexpr std::array<Uniform, 3> kPerDraw{{
        {"uModel", Block::PerDraw, 0, UniformType::Mat4, 64, 1},
        {"uView", Block::PerDraw, 64, UniformType::Mat4, 64, 1},
        {"uProjection", Block::PerDraw, 128, UniformType::Mat4, 64, 1},
    }};

    inline constexpr std::array<Uniform, 3> kUnlitMaterial{{
        {"uAlbedo", Block::Material, 0, UniformType::Vec4, 16, 1},
        {"uHasTexture", Block::Material, 16, UniformType::Int, 4, 1},
        {"uAlphaCutoff", Block::Material, 20, UniformType::Float, 4, 1},
    }};

    inline constexpr std::array<Uniform, 13> kLitMaterial{{
        {"uAlbedo", Block::Material, 0, UniformType::Vec4, 16, 1},
        {"uHasTexture", Block::Material, 16, UniformType::Int, 4, 1},
        {"uMetallic", Block::Material, 20, UniformType::Float, 4, 1},
        {"uSmoothness", Block::Material, 24, UniformType::Float, 4, 1},
        {"uAlphaCutoff", Block::Material, 28, UniformType::Float, 4, 1},
        {"uEmissive", Block::Material, 32, UniformType::Vec3, 12, 1},
        {"uHasEmissiveTexture", Block::Material, 44, UniformType::Int, 4, 1},
        {"uHasOcclusionTexture", Block::Material, 48, UniformType::Int, 4, 1},
        {"uOcclusionStrength", Block::Material, 52, UniformType::Float, 4, 1},
        {"uHasNormalTexture", Block::Material, 56, UniformType::Int, 4, 1},
        {"uNormalScale", Block::Material, 60, UniformType::Float, 4, 1},
        {"uPbr", Block::Material, 64, UniformType::Int, 4, 1},
        {"uHasMetallicRoughnessTexture", Block::Material, 68, UniformType::Int, 4, 1},
    }};

    inline constexpr std::array<Uniform, 7> kTextMaterial{{
        {"uAlbedo", Block::Material, 0, UniformType::Vec4, 16, 1},
        {"uOutline", Block::Material, 16, UniformType::Float, 4, 1},
        {"uOutlineColor", Block::Material, 32, UniformType::Vec4, 16, 1},
        {"uSoftness", Block::Material, 48, UniformType::Float, 4, 1},
        {"uShadowColor", Block::Material, 64, UniformType::Vec4, 16, 1},
        {"uShadowOffset", Block::Material, 80, UniformType::Vec2, 8, 1},
        {"uShadowSoftness", Block::Material, 88, UniformType::Float, 4, 1},
    }};

    /// The lit shader's lighting block: the light structs are std140 structs rounded up to 16 bytes (directional 32,
    /// point 48, spot 64), so the arrays start at 48, 112 and 496 and the block ends at 752
    inline constexpr std::array<Uniform, 9> kLitLighting{{
        {"uCameraPos", Block::Lighting, 0, UniformType::Vec3, 12, 1},
        {"uEncodeOutput", Block::Lighting, 12, UniformType::Int, 4, 1},
        {"uAmbientLight", Block::Lighting, 16, UniformType::Vec3, 12, 1},
        {"uNumDirectionalLights", Block::Lighting, 28, UniformType::Int, 4, 1},
        {"uNumPointLights", Block::Lighting, 32, UniformType::Int, 4, 1},
        {"uNumSpotLights", Block::Lighting, 36, UniformType::Int, 4, 1},
        {"uDirectionalLights", Block::Lighting, 48, UniformType::Struct, 64, kDirectionalLightCount},
        {"uPointLights", Block::Lighting, 112, UniformType::Struct, 384, kPointLightCount},
        {"uSpotLights", Block::Lighting, 496, UniformType::Struct, 256, kSpotLightCount},
    }};
    static_assert(kLitLighting.back().offset + kLitLighting.back().size == kLightingBlockSize);

    /// One member of a light struct inside the lighting block: its std140 offset from the start of the array element
    struct Member
    {
        std::string_view name;
        std::uint32_t offset;
        UniformType type;
    };

    // The light structs (std140: a vec3 aligns to 16 and a float may follow it in the same 16 bytes; a struct is rounded up
    // to 16 bytes, so the array strides are 32, 48 and 64). They are the declarations in lit.frag, member by member
    inline constexpr std::array<Member, 3> kDirectionalLightMembers{{
        {"direction", 0, UniformType::Vec3},
        {"color", 16, UniformType::Vec3},
        {"intensity", 28, UniformType::Float},
    }};
    inline constexpr std::uint32_t kDirectionalLightStride = 32;

    inline constexpr std::array<Member, 5> kPointLightMembers{{
        {"position", 0, UniformType::Vec3},
        {"color", 16, UniformType::Vec3},
        {"intensity", 28, UniformType::Float},
        {"range", 32, UniformType::Float},
        {"attenuation", 36, UniformType::Float},
    }};
    inline constexpr std::uint32_t kPointLightStride = 48;

    inline constexpr std::array<Member, 7> kSpotLightMembers{{
        {"position", 0, UniformType::Vec3},
        {"direction", 16, UniformType::Vec3},
        {"color", 32, UniformType::Vec3},
        {"intensity", 44, UniformType::Float},
        {"range", 48, UniformType::Float},
        {"cosInner", 52, UniformType::Float},
        {"cosOuter", 56, UniformType::Float},
    }};
    inline constexpr std::uint32_t kSpotLightStride = 64;

    /// The offset of a lighting-block member of the lit shader, resolved at compile time: a name that is not in the table
    /// does not compile, so the code that fills the block cannot carry a second, hand-typed copy of an offset
    [[nodiscard]] consteval std::uint32_t LightingOffset(const std::string_view name)
    {
        for (const Uniform& uniform : kLitLighting)
        {
            if (uniform.name == name)
            {
                return uniform.offset;
            }
        }
        throw "not a member of the lit shader's lighting block";
    }

    /// The offset of a member inside one element of a light array, resolved at compile time
    template <std::size_t N>
    [[nodiscard]] consteval std::uint32_t MemberOffset(const std::array<Member, N>& members, const std::string_view name)
    {
        for (const Member& member : members)
        {
            if (member.name == name)
            {
                return member.offset;
            }
        }
        throw "not a member of the light struct";
    }

    // The array entries of the lighting table and the struct tables agree (count x stride = the array's size)
    static_assert(kDirectionalLightStride * kDirectionalLightCount == kLitLighting[6].size);
    static_assert(kPointLightStride * kPointLightCount == kLitLighting[7].size);
    static_assert(kSpotLightStride * kSpotLightCount == kLitLighting[8].size);
    static_assert(kDirectionalLightMembers.back().offset + 4 <= kDirectionalLightStride);
    static_assert(kPointLightMembers.back().offset + 4 <= kPointLightStride);
    static_assert(kSpotLightMembers.back().offset + 4 <= kSpotLightStride);

    /// What a new material of a shader holds before anything is set: the OpenGL defaults (OpenGLMaterial's constructor and
    /// OpenGLRenderer::CreateMaterial), so a Vulkan material draws what an OpenGL one does. `values` holds the vec4 or vec3
    /// components, or the one float or int (an int is the value cast). uHasTexture is not here: it depends on whether a
    /// texture was given at creation
    struct MaterialDefault
    {
        std::string_view name;
        std::array<float, 4> values;
    };

    inline constexpr std::array<MaterialDefault, 2> kUnlitMaterialDefaults{{
        {"uAlbedo", {1.0f, 1.0f, 1.0f, 1.0f}},
        {"uAlphaCutoff", {0.0f, 0.0f, 0.0f, 0.0f}},
    }};

    inline constexpr std::array<MaterialDefault, 12> kLitMaterialDefaults{{
        {"uAlbedo", {1.0f, 1.0f, 1.0f, 1.0f}},
        {"uMetallic", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uSmoothness", {0.5f, 0.0f, 0.0f, 0.0f}},
        {"uAlphaCutoff", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uEmissive", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uHasEmissiveTexture", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uHasOcclusionTexture", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uOcclusionStrength", {1.0f, 0.0f, 0.0f, 0.0f}},
        {"uHasNormalTexture", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uNormalScale", {1.0f, 0.0f, 0.0f, 0.0f}},
        {"uPbr", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uHasMetallicRoughnessTexture", {0.0f, 0.0f, 0.0f, 0.0f}},
    }};

    /// The text shader's material defaults (OpenGL's CreateMaterial gives the text shader no effects; uAlbedo is white)
    inline constexpr std::array<MaterialDefault, 7> kTextMaterialDefaults{{
        {"uAlbedo", {1.0f, 1.0f, 1.0f, 1.0f}},
        {"uOutline", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uOutlineColor", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uSoftness", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uShadowColor", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uShadowOffset", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"uShadowSoftness", {0.0f, 0.0f, 0.0f, 0.0f}},
    }};

    inline constexpr std::array<Sampler, 1> kBaseSampler{{{"uTexture", 1}}};
    inline constexpr std::array<Sampler, 5> kLitSamplers{{
        {"uTexture", 1},
        {"uEmissiveTexture", 2},
        {"uOcclusionTexture", 3},
        {"uNormalTexture", 4},
        {"uMetallicRoughnessTexture", 5},
    }};

    // ---- Per-shader views -------------------------------------------------------------------------------------------
    [[nodiscard]] constexpr std::span<const Uniform> MaterialUniforms(const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::Unlit: return kUnlitMaterial;
        case BuiltInShader::Lit: return kLitMaterial;
        case BuiltInShader::Text: return kTextMaterial;
        }
        return {};
    }

    [[nodiscard]] constexpr std::span<const MaterialDefault> MaterialDefaults(const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::Unlit: return kUnlitMaterialDefaults;
        case BuiltInShader::Lit: return kLitMaterialDefaults;
        case BuiltInShader::Text: return kTextMaterialDefaults;
        }
        return {};
    }

    /// The lighting block of a shader: only the lit shader has one
    [[nodiscard]] constexpr std::span<const Uniform> LightingUniforms(const BuiltInShader shader)
    {
        return shader == BuiltInShader::Lit ? std::span<const Uniform>(kLitLighting) : std::span<const Uniform>();
    }

    [[nodiscard]] constexpr std::span<const Attribute> Attributes(const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::Unlit: return kUnlitAttributes;
        case BuiltInShader::Lit: return kLitAttributes;
        case BuiltInShader::Text: return kTextAttributes;
        }
        return {};
    }

    [[nodiscard]] constexpr std::span<const Sampler> Samplers(const BuiltInShader shader)
    {
        return shader == BuiltInShader::Lit ? std::span<const Sampler>(kLitSamplers) : std::span<const Sampler>(kBaseSampler);
    }

    /// The size in bytes of the shader's material block (what the GLSL block takes; the descriptor covers
    /// kMaterialBlockRange)
    [[nodiscard]] constexpr std::uint32_t MaterialBlockSize(const BuiltInShader shader)
    {
        const std::span<const Uniform> uniforms = MaterialUniforms(shader);
        return uniforms.empty() ? 0 : uniforms.back().offset + uniforms.back().size;
    }

    /// The material-block uniform of `shader` called `name`, or nullptr
    [[nodiscard]] constexpr const Uniform* FindMaterialUniform(const BuiltInShader shader, const std::string_view name)
    {
        for (const Uniform& uniform : MaterialUniforms(shader))
        {
            if (uniform.name == name)
            {
                return &uniform;
            }
        }
        return nullptr;
    }

    [[nodiscard]] constexpr std::string_view ShaderName(const BuiltInShader shader)
    {
        switch (shader)
        {
        case BuiltInShader::Unlit: return "unlit";
        case BuiltInShader::Lit: return "lit";
        case BuiltInShader::Text: return "text";
        }
        return "";
    }

    static_assert(MaterialBlockSize(BuiltInShader::Unlit) <= kMaterialBlockRange);
    static_assert(MaterialBlockSize(BuiltInShader::Lit) <= kMaterialBlockRange);
    static_assert(MaterialBlockSize(BuiltInShader::Text) <= kMaterialBlockRange);
}
