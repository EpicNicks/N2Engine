#include "renderer/common/BlockMaterial.hpp"

#include <cstring>
#include <string>
#include <utility>

using namespace Renderer::Gpu;
using namespace Renderer::Gpu::ShaderInterface;
using Renderer::Common::IShader;
using Renderer::Common::ITexture;

namespace
{
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
}

// ---------------------------------------------------------------------------------------------------------------------
// BlockMaterial
// ---------------------------------------------------------------------------------------------------------------------

BlockMaterial::BlockMaterial(const BuiltInShader shaderKind, IShader* owner, ITexture* baseTexture, Warn warn,
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

const Uniform* BlockMaterial::Find(const std::string& name, const UniformType expected, const char* setter) const
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

void BlockMaterial::WriteFloats(const std::uint32_t offset, const float* values, const std::uint32_t count)
{
    if (offset + count * sizeof(float) <= _block.size())
    {
        std::memcpy(_block.data() + offset, values, count * sizeof(float));
    }
}

void BlockMaterial::SetInt(const std::string& name, const int value)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Int, "SetInt"))
    {
        const std::int32_t stored = value;
        std::memcpy(_block.data() + uniform->offset, &stored, sizeof(stored));
    }
}

void BlockMaterial::SetFloat(const std::string& name, const float value)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Float, "SetFloat"))
    {
        WriteFloats(uniform->offset, &value, 1);
    }
}

void BlockMaterial::SetVec2(const std::string& name, const float x, const float y)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec2, "SetVec2"))
    {
        const float values[2] = {x, y};
        WriteFloats(uniform->offset, values, 2);
    }
}

void BlockMaterial::SetVec2(const std::string& name, N2Engine::Math::Vector2& value)
{
    SetVec2(name, value.x, value.y);
}

void BlockMaterial::SetVec3(const std::string& name, const float x, const float y, const float z)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec3, "SetVec3"))
    {
        const float values[3] = {x, y, z};
        WriteFloats(uniform->offset, values, 3);
    }
}

void BlockMaterial::SetVec3(const std::string& name, N2Engine::Math::Vector3& value)
{
    SetVec3(name, value.x, value.y, value.z);
}

void BlockMaterial::SetVec4(const std::string& name, const float x, const float y, const float z, const float w)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec4, "SetVec4"))
    {
        const float values[4] = {x, y, z, w};
        WriteFloats(uniform->offset, values, 4);
    }
}

void BlockMaterial::SetVec4(const std::string& name, N2Engine::Math::Vector4& value)
{
    SetVec4(name, value.x, value.y, value.z, value.w);
}

void BlockMaterial::SetColor(const std::string& name, const float r, const float g, const float b, const float a)
{
    std::lock_guard lock(_mutex);
    if (const Uniform* uniform = Find(name, UniformType::Vec4, "SetColor"))
    {
        const float values[4] = {r, g, b, a};
        WriteFloats(uniform->offset, values, 4);
    }
}

void BlockMaterial::SetTexture(ITexture* texture)
{
    // The id is asked for before this material's lock is taken (the resolver takes its own leaf lock)
    const std::uint64_t id = (_textureIdOf && texture != nullptr) ? _textureIdOf(texture) : 0;
    std::lock_guard lock(_mutex);
    _texture = texture;
    _textureId = id;
}

void BlockMaterial::SetAuxTexture(const Common::AuxTexture which, ITexture* texture)
{
    const std::uint64_t id = (_textureIdOf && texture != nullptr) ? _textureIdOf(texture) : 0;
    std::lock_guard lock(_mutex);
    if (const auto slot = static_cast<std::size_t>(which); slot < _aux.size())
    {
        _aux[slot] = texture;
        _auxIds[slot] = id;
    }
}

ITexture* BlockMaterial::GetAuxTexture(const Common::AuxTexture which) const
{
    std::lock_guard lock(_mutex);
    const auto slot = static_cast<std::size_t>(which);
    return slot < _aux.size() ? _aux[slot] : nullptr;
}

ITexture* BlockMaterial::GetTexture() const
{
    std::lock_guard lock(_mutex);
    return _texture;
}

void BlockMaterial::SetTextureIdForTest(const std::size_t slot, const std::uint64_t id)
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

BlockMaterial::Snapshot BlockMaterial::TakeSnapshot() const
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
