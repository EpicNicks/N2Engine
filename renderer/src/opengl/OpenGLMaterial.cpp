#include <iostream>

#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/opengl/OpenGLMaterial.hpp"
#include "renderer/common/UserShaderGl.hpp"

using namespace Renderer::OpenGL;

OpenGLMaterial::OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, Common::ITexture *texture)
    : _shader(shader), _texture(texture ? static_cast<OpenGLTexture *>(texture) : nullptr)
{
    SetColor("uAlbedo", 1, 1, 1, 1);
}

OpenGLMaterial::OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, Common::ITexture *texture,
                               std::shared_ptr<const Gpu::MaterialLayout> userLayout)
    : _shader(std::move(shader)), _layout(std::move(userLayout)),
      _texture(texture ? static_cast<OpenGLTexture *>(texture) : nullptr)
{
    // No uAlbedo: the declared parameters are all a user program has. The defaults are what the manifest says
    if (!_layout)
    {
        return;
    }
    using Gpu::ShaderInterface::UniformType;
    for (const Gpu::MaterialLayout::Default &entry : _layout->defaults)
    {
        const Gpu::ShaderInterface::Uniform &uniform = _layout->uniforms[entry.uniform];
        const std::string name(uniform.name);
        const std::array<float, 4> &v = entry.values;
        switch (uniform.type)
        {
        case UniformType::Float: _floats[name] = v[0]; break;
        case UniformType::Int: _ints[name] = static_cast<int>(v[0]); break;
        case UniformType::Vec2: _vec2s[name] = {v[0], v[1]}; break;
        case UniformType::Vec3: _vec3s[name] = {v[0], v[1], v[2]}; break;
        case UniformType::Vec4: _vec4s[name] = {v[0], v[1], v[2], v[3]}; break;
        case UniformType::Mat3: _mat3s[name] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; break;
        case UniformType::Mat4: _mat4s[name] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; break;
        default: break;
        }
    }
}

bool OpenGLMaterial::AcceptUser(const std::string &name, const Gpu::ShaderInterface::UniformType expected, const char *setter)
{
    if (!_layout)
    {
        return true;
    }
    for (const Gpu::ShaderInterface::Uniform &uniform : _layout->uniforms)
    {
        if (uniform.name == name)
        {
            if (uniform.type == expected)
            {
                return true;
            }
            if (_warned.insert(name).second)
            {
                std::cerr << "material uniform '" << name << "' of the " << _layout->shaderName << " shader is set with the wrong type ("
                          << setter << "): ignored" << std::endl;
            }
            return false;
        }
    }
    if (_warned.insert(name).second)
    {
        std::cerr << "material uniform '" << name << "' is not in the " << _layout->shaderName << " shader's parameters: ignored"
                  << std::endl;
    }
    return false;
}

void OpenGLMaterial::SetFloat(const std::string &name, float value)
{
    if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Float, "SetFloat"))
        _floats[name] = value;
}

void OpenGLMaterial::SetInt(const std::string &name, int value)
{
    if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Int, "SetInt"))
        _ints[name] = value;
}

void OpenGLMaterial::SetVec2(const std::string &name, float x, float y)
{
    if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Vec2, "SetVec2"))
        _vec2s[name] = {x, y};
}

void OpenGLMaterial::SetVec2(const std::string &name, N2Engine::Math::Vector2 &value)
{
    SetVec2(name, value.x, value.y);
}

void OpenGLMaterial::SetVec3(const std::string &name, float x, float y, float z)
{
    if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Vec3, "SetVec3"))
        _vec3s[name] = {x, y, z};
}

void OpenGLMaterial::SetVec3(const std::string &name, N2Engine::Math::Vector3 &value)
{
    SetVec3(name, value.x, value.y, value.z);
}

void OpenGLMaterial::SetVec4(const std::string &name, float x, float y, float z, float w)
{
    if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Vec4, "SetVec4"))
        _vec4s[name] = {x, y, z, w};
}

void OpenGLMaterial::SetVec4(const std::string &name, N2Engine::Math::Vector4 &value)
{
    SetVec4(name, value.x, value.y, value.z, value.w);
}

void Renderer::OpenGL::OpenGLMaterial::SetColor(const std::string &name, float r, float g, float b, float a)
{
    SetVec4(name, r, g, b, a);
}

void OpenGLMaterial::SetMatrix(const std::string &name, const float *rowMajor, const unsigned dimension)
{
    if (!_layout || rowMajor == nullptr || (dimension != 3 && dimension != 4))
    {
        return; // only a user shader has matrix parameters
    }
    if (dimension == 3)
    {
        if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Mat3, "SetMat3"))
            std::copy(rowMajor, rowMajor + 9, _mat3s[name].begin());
    }
    else if (AcceptUser(name, Gpu::ShaderInterface::UniformType::Mat4, "SetMat4"))
    {
        std::copy(rowMajor, rowMajor + 16, _mat4s[name].begin());
    }
}
void OpenGLMaterial::SetTexture(Renderer::Common::ITexture *texture)
{
    _texture = texture ? static_cast<OpenGLTexture *>(texture) : nullptr;
}

void OpenGLMaterial::SetAuxTexture(Common::AuxTexture which, Common::ITexture *texture)
{
    const auto slot = static_cast<std::size_t>(which);
    if (slot >= kAuxTextureCount)
        return;
    // An invalid texture is stored as none, so what ApplyUniforms reads (the has-texture flags) is what is bound
    _auxTextures[slot] = texture && texture->IsValid() ? static_cast<OpenGLTexture *>(texture) : nullptr;
    // The sampler's texture unit (the base colour texture is unit 0); sent with the other ints on every draw. A user program has no
    // such sampler (its n2_tex<slot> samplers are set by Apply)
    if (!_layout)
    {
        _ints[AuxSamplerName(which)] = AuxTextureUnit(which);
    }
}

Renderer::Common::ITexture *OpenGLMaterial::GetAuxTexture(Common::AuxTexture which) const
{
    return GetAuxOpenGLTexture(which);
}

OpenGLTexture *OpenGLMaterial::GetAuxOpenGLTexture(Common::AuxTexture which) const
{
    const auto slot = static_cast<std::size_t>(which);
    return slot < kAuxTextureCount ? _auxTextures[slot] : nullptr;
}

const char *OpenGLMaterial::AuxSamplerName(Common::AuxTexture which)
{
    switch (which)
    {
    case Common::AuxTexture::Emissive:
        return "uEmissiveTexture";
    case Common::AuxTexture::Occlusion:
        return "uOcclusionTexture";
    case Common::AuxTexture::Normal:
        return "uNormalTexture";
    case Common::AuxTexture::MetallicRoughness:
        return "uMetallicRoughnessTexture";
    }
    return "";
}

int OpenGLMaterial::AuxTextureUnit(Common::AuxTexture which)
{
    // Unit 0 is the base colour texture
    return 1 + static_cast<int>(which);
}

void OpenGLMaterial::Apply()
{
    if (!_shader || !_shader->IsValid())
        return;

    // Bind the shader first
    _shader->Bind();

    // Apply all stored properties to the shader
    for (const auto &[name, value] : _floats)
    {
        _shader->SetFloat(name, value);
    }

    for (const auto &[name, value] : _ints)
    {
        _shader->SetInt(name, value);
    }

    for (const auto &[name, value] : _vec2s)
    {
        _shader->SetVec2(name, value[0], value[1]);
    }

    for (const auto &[name, value] : _vec3s)
    {
        _shader->SetVec3(name, value[0], value[1], value[2]);
    }

    for (const auto &[name, value] : _vec4s)
    {
        _shader->SetVec4(name, value[0], value[1], value[2], value[3]);
    }

    for (const auto &[name, value] : _mat3s)
    {
        _shader->SetMatrixRowMajor(name, value.data(), 3);
    }

    for (const auto &[name, value] : _mat4s)
    {
        _shader->SetMatrixRowMajor(name, value.data(), 4);
    }

    // A user program's samplers: slot N reads texture unit N (the base colour texture is unit 0, the AuxTexture slots 1-4)
    if (_layout)
    {
        for (unsigned slot = 0; slot < _layout->textureDeclared.size(); ++slot)
        {
            if (_layout->textureDeclared[slot])
            {
                _shader->SetInt(Common::UserGl::TextureSamplerName(slot), static_cast<int>(slot));
            }
        }
    }
}