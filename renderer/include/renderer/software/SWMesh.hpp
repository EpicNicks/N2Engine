#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "renderer/common/IMesh.hpp"
#include "renderer/common/RenderTypes.hpp"

namespace Renderer::Software
{
    class SWMesh : public Common::IMesh
    {
    public:
        std::vector<Common::Vertex> vertices;
        std::vector<uint32_t> indices;
        /// Whether any vertex colour isn't white (1, 1, 1, 1). Only then do the unlit and lit shaders interpolate
        /// and multiply it, so all-white meshes shade exactly as before vertex colour was read. Kept up to date by
        /// SetData, which SoftwareRenderer::CreateMesh and UpdateMesh use, so draws don't scan the vertices.
        bool hasVertexColors = false;

        /// Replaces the geometry and refreshes hasVertexColors
        void SetData(const Common::MeshData &meshData)
        {
            vertices = meshData.vertices;
            indices = meshData.indices;
            hasVertexColors = std::ranges::any_of(vertices, [](const Common::Vertex &vertex)
            {
                return vertex.color[0] != 1.f || vertex.color[1] != 1.f || vertex.color[2] != 1.f ||
                       vertex.color[3] != 1.f;
            });
        }

        [[nodiscard]] bool IsValid() const override { return !vertices.empty() && !indices.empty(); }
        [[nodiscard]] uint32_t GetIndexCount() const override { return static_cast<uint32_t>(indices.size()); }
        [[nodiscard]] uint32_t GetVertexCount() const override { return static_cast<uint32_t>(vertices.size()); }
    };
}
