#pragma once

#include <cstdint>

namespace Renderer::Common
{
    class IMesh
    {
    public:
        virtual ~IMesh() = default;

        virtual bool IsValid() const = 0;
        virtual uint32_t GetIndexCount() const = 0;
        virtual uint32_t GetVertexCount() const = 0;
    };
}