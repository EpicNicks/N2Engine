#pragma once

#include <vector>
#include <cstdint>

namespace Renderer::Common
{
    struct Vertex
    {
        float position[3];
        float normal[3];
        float texCoord[2];
        float color[4];
    };

    struct MeshData
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

    /**
     * A run of a mesh's indices, drawn on its own by IRenderer::DrawMesh(..., IndexRange): `count` indices
     * starting at index `first` (a submesh). Both are counted in indices, not triangles or bytes; a range of
     * whole triangles has both a multiple of 3.
     */
    struct IndexRange
    {
        uint32_t first = 0;
        uint32_t count = 0;

        friend constexpr bool operator==(const IndexRange &, const IndexRange &) = default;
    };

    struct Transform
    {
        float model[16];
        float view[16];
        float projection[16];
    };
}