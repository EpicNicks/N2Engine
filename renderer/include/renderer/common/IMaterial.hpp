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

        [[nodiscard]] virtual IShader* GetShader() const = 0;
        [[nodiscard]] virtual ITexture* GetTexture() const = 0;
        [[nodiscard]] virtual bool IsValid() const = 0;
    };
}
