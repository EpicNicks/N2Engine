// renderer/opengl/OpenGLMaterial.hpp
#pragma once

#include <memory>
#include <unordered_map>
#include <array>
#include <string>

#include "renderer/common/IMaterial.hpp"
#include "renderer/common/ITexture.hpp"
#include "renderer/opengl/OpenGLShader.hpp"
#include "renderer/opengl/OpenGLTexture.hpp"

namespace Renderer::OpenGL
{
    class OpenGLMaterial : public Common::IMaterial
    {
    public:
        OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, Common::ITexture *texture = nullptr);
        ~OpenGLMaterial() override = default;

        // IMaterial interface implementation
        void SetFloat(const std::string &name, float value) override;
        void SetInt(const std::string &name, int value) override;
        void SetVec2(const std::string &name, float x, float y) override;
        void SetVec2(const std::string &name, N2Engine::Math::Vector2 &value) override;
        void SetVec3(const std::string &name, float x, float y, float z) override;
        void SetVec3(const std::string &name, N2Engine::Math::Vector3 &value) override;
        void SetVec4(const std::string &name, float x, float y, float z, float w) override;
        void SetVec4(const std::string &name, N2Engine::Math::Vector4 &value) override;
        void SetColor(const std::string &name, float r, float g, float b, float a) override;

        bool IsValid() const override { return _shader && _shader->IsValid(); }

        // Internal use by renderer
        void Apply(); // Binds shader and applies all properties
        OpenGLShader *GetShader() const { return _shader.get(); }
        OpenGLTexture *GetTexture() const { return _texture; }
        void SetTexture(Common::ITexture *texture) override;

    private:
        std::shared_ptr<OpenGLShader> _shader;
        OpenGLTexture *_texture;

        // Property storage
        std::unordered_map<std::string, float> _floats;
        std::unordered_map<std::string, int> _ints;
        std::unordered_map<std::string, std::array<float, 2>> _vec2s;
        std::unordered_map<std::string, std::array<float, 3>> _vec3s;
        std::unordered_map<std::string, std::array<float, 4>> _vec4s;
    };
}