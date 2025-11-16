#include "renderer/opengl/OpenGLMaterial.hpp"

using namespace Renderer::OpenGL;

OpenGLMaterial::OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, uint32_t shaderId, uint32_t textureId)
    : _shader(shader), _shaderId(shaderId), _textureId(textureId)
{
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

void OpenGLMaterial::SetVec3(const std::string &name, float x, float y, float z)
{
    _vec3s[name] = {x, y, z};
}

void OpenGLMaterial::SetVec4(const std::string &name, float x, float y, float z, float w)
{
    _vec4s[name] = {x, y, z, w};
}

void Renderer::OpenGL::OpenGLMaterial::SetColor(const std::string &name, float r, float g, float b, float a)
{
    SetVec4(name, r, g, b, a);
}
void OpenGLMaterial::SetTexture(uint32_t textureId)
{
    _textureId = textureId;
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