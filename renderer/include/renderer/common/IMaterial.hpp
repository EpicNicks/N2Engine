#pragma once

#include <string>


namespace N2Engine::Math
{
    struct Vector2;
    class Vector3;
    struct Vector4;
}

namespace Renderer::Common
{
    class ITexture;
    class IShader;

    /// The textures a material can hold besides its base colour texture (SetTexture)
    enum class AuxTexture : unsigned char
    {
        Emissive,          ///< the standard lit shader adds its rgb times uEmissive
        Occlusion,         ///< the standard lit shader scales the ambient light by its red channel (see uOcclusionStrength)
        Normal,            ///< the standard lit shader perturbs the surface normal by it, in tangent space (see uNormalScale)
        MetallicRoughness  ///< PBR (uPbr): green times roughness, blue times metallic (glTF's packing)
    };

    class IMaterial
    {
    public:
        virtual ~IMaterial() = default;

        virtual void SetInt(const std::string &name, int value) = 0;
        virtual void SetFloat(const std::string &name, float value) = 0;
        virtual void SetVec2(const std::string &name, float x, float y) = 0;
        virtual void SetVec2(const std::string &name, N2Engine::Math::Vector2 &value) = 0;
        virtual void SetVec3(const std::string &name, float x, float y, float z) = 0;
        virtual void SetVec3(const std::string &name, N2Engine::Math::Vector3 &value) = 0;
        virtual void SetVec4(const std::string &name, float x, float y, float z, float w) = 0;
        virtual void SetVec4(const std::string &name, N2Engine::Math::Vector4 &value) = 0;
        virtual void SetColor(const std::string &name, float r, float g, float b, float a) = 0;
        virtual void SetTexture(ITexture *texture) = 0;
        /// Sets (or, with null, clears) one of the extra textures. The default does nothing, for a backend or a
        /// test fake without them; GetAuxTexture then reads null.
        virtual void SetAuxTexture(AuxTexture, ITexture *) {}
        [[nodiscard]] virtual ITexture* GetAuxTexture(AuxTexture) const { return nullptr; }
        /// Sets one of the five texture slots by number: 0 is the base texture (SetTexture), 1-4 the AuxTexture slots in
        /// enum order. A user shader's texture parameters are generic slots, so this is how a caller that knows a
        /// parameter's slot (from the cooked manifest) fills it without a backend interface change. A slot past 4 does
        /// nothing
        virtual void SetTextureSlot(unsigned slot, ITexture *texture)
        {
            if (slot == 0)
            {
                SetTexture(texture);
            }
            else if (slot <= 4)
            {
                SetAuxTexture(static_cast<AuxTexture>(slot - 1), texture);
            }
        }

        /// Whether this material is of a USER shader (IRenderer::CreateUserMaterial): its values are the shader's declared
        /// parameters, not the standard shaders' uniforms (uAlbedo ...). The engine's Material writes the one set or the other
        /// (Material::ApplyUniforms), so a material that fell back to a standard shader gets the standard uniforms. False by default
        [[nodiscard]] virtual bool IsUserShaderMaterial() const { return false; }

        [[nodiscard]] virtual IShader* GetShader() const = 0;
        [[nodiscard]] virtual ITexture* GetTexture() const = 0;
        [[nodiscard]] virtual bool IsValid() const = 0;
    };
}
