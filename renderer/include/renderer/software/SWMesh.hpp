#pragma once

#include "renderer/common/IMesh.hpp"
#include "renderer/common/RenderTypes.hpp"

namespace Renderer::Software
{
    class SWMesh : public Common::IMesh
    {
    public:
        std::vector<Common::Vertex> vertices;
        std::vector<uint32_t> indices;

        [[nodiscard]] bool IsValid() const override { return !vertices.empty() && !indices.empty(); }
        [[nodiscard]] uint32_t GetIndexCount() const override { return static_cast<uint32_t>(indices.size()); }
        [[nodiscard]] uint32_t GetVertexCount() const override { return static_cast<uint32_t>(vertices.size()); }
    };
}
