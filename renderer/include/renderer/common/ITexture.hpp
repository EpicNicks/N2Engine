#pragma once

#include <cstdint>

namespace Renderer::Common
{
    class ITexture
    {
    public:
        virtual ~ITexture() = default;

        virtual bool IsValid() const = 0;
        virtual uint32_t GetWidth() const = 0;
        virtual uint32_t GetHeight() const = 0;
        virtual uint32_t GetChannels() const = 0;
    };
}