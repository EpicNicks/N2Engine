#pragma once

#include <string>
#include <cstdint>

namespace Renderer::Common
{
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
        virtual void SetTexture(uint32_t textureId) = 0;

        virtual uint32_t GetShaderId() const = 0;
        virtual uint32_t GetTextureId() const = 0;
        virtual bool IsValid() const = 0;
    };
}