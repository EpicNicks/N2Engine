#include <math/Vector2.hpp>
#include <math/Vector3.hpp>
#include <math/Vector4.hpp>

#include "renderer/opengl/OpenGLMaterial.hpp"

using namespace Renderer::OpenGL;

OpenGLMaterial::OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, Common::ITexture *texture)
    : _shader(shader), _texture(texture ? static_cast<OpenGLTexture *>(texture) : nullptr)
{
    SetColor("uAlbedo", 1, 1, 1, 1);
}

void OpenGLMaterial::SetFloat(const std::string &name, float value)
{
    _floats[name] = value;
}

void OpenGLMaterial::SetInt(const std::string &name, int value)
{
    _ints[name] = value;
}

void OpenGLMaterial::SetVec2(const std::string &name, float x, float y)
{
    _vec2s[name] = {x, y};
}

void OpenGLMaterial::SetVec2(const std::string &name, N2Engine::Math::Vector2 &value)
{
    _vec2s[name] = {value.x, value.y};
}

void OpenGLMaterial::SetVec3(const std::string &name, float x, float y, float z)
{
    _vec3s[name] = {x, y, z};
}

void OpenGLMaterial::SetVec3(const std::string &name, N2Engine::Math::Vector3 &value)
{
    _vec3s[name] = {value.x, value.y, value.z};
}

void OpenGLMaterial::SetVec4(const std::string &name, float x, float y, float z, float w)
{
    _vec4s[name] = {x, y, z, w};
}

void OpenGLMaterial::SetVec4(const std::string &name, N2Engine::Math::Vector4 &value)
{
    _vec4s[name] = {value.x, value.y, value.z, value.w};
}

void Renderer::OpenGL::OpenGLMaterial::SetColor(const std::string &name, float r, float g, float b, float a)
{
    SetVec4(name, r, g, b, a);
}
void OpenGLMaterial::SetTexture(Renderer::Common::ITexture *texture)
{
    _texture = texture ? static_cast<OpenGLTexture *>(texture) : nullptr;
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
}