// renderer/opengl/OpenGLMaterial.hpp
#pragma once

#include <memory>
#include <set>
#include <unordered_map>
#include <array>
#include <string>

#include "renderer/common/BlockMaterial.hpp"
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
        /// A material of a user shader (stage 4): it starts at the manifest's defaults, takes only the declared parameter names and
        /// types (an unknown name or the wrong setter warns once and is ignored, as on Vulkan and WebGPU), and sets the shader's
        /// texture samplers (n2_tex<slot> on unit <slot>) on Apply
        OpenGLMaterial(std::shared_ptr<OpenGLShader> shader, Common::ITexture *texture,
                       std::shared_ptr<const Gpu::MaterialLayout> userLayout);
        [[nodiscard]] bool IsUserShader() const { return _layout != nullptr; }
        [[nodiscard]] const Gpu::MaterialLayout *UserLayout() const { return _layout.get(); }
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
        void SetMatrix(const std::string &name, const float *rowMajor, unsigned dimension) override;

        bool IsValid() const override { return _shader && _shader->IsValid(); }
        [[nodiscard]] bool IsUserShaderMaterial() const override { return IsUserShader(); }

        // Internal use by renderer
        void Apply(); // Binds shader and applies all properties
        OpenGLShader *GetShader() const { return _shader.get(); }
        OpenGLTexture *GetTexture() const { return _texture; }
        void SetTexture(Common::ITexture *texture) override;
        void SetAuxTexture(Common::AuxTexture which, Common::ITexture *texture) override;
        Common::ITexture *GetAuxTexture(Common::AuxTexture which) const override;
        /// How many extra textures a material can hold (AuxTexture's values), and the texture unit and sampler uniform
        /// of each: emissive on unit 1, occlusion 2, normal 3, metallic-roughness 4 (the base colour texture is unit 0)
        static constexpr std::size_t kAuxTextureCount = 4;
        [[nodiscard]] static int AuxTextureUnit(Common::AuxTexture which);
        [[nodiscard]] static const char *AuxSamplerName(Common::AuxTexture which);
        /// The extra texture, or null (GetAuxTexture, as the OpenGL type)
        [[nodiscard]] OpenGLTexture *GetAuxOpenGLTexture(Common::AuxTexture which) const;

    private:
        /// The declared parameter `name` when it has the type `expected` (a user material), else a warning once per name and false.
        /// Always true for a built-in material
        bool AcceptUser(const std::string &name, Gpu::ShaderInterface::UniformType expected, const char *setter);

        std::shared_ptr<OpenGLShader> _shader;
        std::shared_ptr<const Gpu::MaterialLayout> _layout; // null for a built-in shader's material
        std::set<std::string> _warned;
        OpenGLTexture *_texture;
        std::array<OpenGLTexture *, kAuxTextureCount> _auxTextures{};

        // Property storage
        std::unordered_map<std::string, float> _floats;
        std::unordered_map<std::string, int> _ints;
        std::unordered_map<std::string, std::array<float, 2>> _vec2s;
        std::unordered_map<std::string, std::array<float, 3>> _vec3s;
        std::unordered_map<std::string, std::array<float, 4>> _vec4s;
        std::unordered_map<std::string, std::array<float, 9>> _mat3s; // a user shader's mat3 parameters, row-major
        std::unordered_map<std::string, std::array<float, 16>> _mat4s; // and mat4
    };
}