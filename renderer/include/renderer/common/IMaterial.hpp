#pragma once

#include <string>
#include <cstdint>

namespace Renderer::Common
{
    class ITexture;
    class IShader;

    class IMaterial
    {
    public:
        virtual ~IMaterial() = default;

        virtual void SetInt(const std::string &name, int value) = 0;
        virtual void SetFloat(const std::string &name, float value) = 0;
        virtual void SetVec2(const std::string &name, float x, float y) = 0;
        virtual void SetVec3(const std::string &name, float x, float y, float z) = 0;
        virtual void SetVec4(const std::string &name, float x, float y, float z, float w) = 0;
        virtual void SetColor(const std::string &name, float x, float y, float z, float w) = 0;
        virtual void SetTexture(ITexture *texture) = 0;

        virtual IShader *GetShader() const = 0;
        virtual ITexture *GetTexture() const = 0;
        virtual bool IsValid() const = 0;
    };
}