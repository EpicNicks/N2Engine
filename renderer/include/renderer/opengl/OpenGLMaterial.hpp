// renderer/opengl/OpenGLMaterial.hpp
#pragma once

#include <memory>
#include <unordered_map>
#include <array>
#include <string>

#include "renderer/common/IMaterial.hpp"
#include "renderer/opengl/OpenGLShader.hpp"

namespace Renderer::OpenGL
{
    class OpenGLMaterial : public Common::IMaterial
    {
    public:
        OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, uint32_t shaderId, uint32_t textureId = 0);
        ~OpenGLMaterial() override = default;

        // IMaterial interface implementation
        void SetFloat(const std::string &name, float value) override;
        void SetInt(const std::string &name, int value) override;
        void SetVec2(const std::string &name, float x, float y) override;
        void SetVec3(const std::string &name, float x, float y, float z) override;
        void SetVec4(const std::string &name, float x, float y, float z, float w) override;
        void SetColor(const std::string &name, float r, float g, float b, float a) override;
        void SetTexture(uint32_t textureId) override;

        uint32_t GetShaderId() const override { return _shaderId; }
        uint32_t GetTextureId() const override { return _textureId; }
        bool IsValid() const override { return _shader && _shader->IsValid(); }

        // Internal use by renderer
        void Apply(); // Binds shader and applies all properties
        OpenGLShader *GetShader() const { return _shader.get(); }

    private:
        std::shared_ptr<OpenGLShader> _shader;
        uint32_t _shaderId; // Store ID for GetShaderId()
        uint32_t _textureId;

        // Property storage
        std::unordered_map<std::string, float> _floats;
        std::unordered_map<std::string, int> _ints;
        std::unordered_map<std::string, std::array<float, 2>> _vec2s;
        std::unordered_map<std::string, std::array<float, 3>> _vec3s;
        std::unordered_map<std::string, std::array<float, 4>> _vec4s;
    };
}