#pragma once

#include <vector>
#include <cstdint>

namespace Renderer::Common
{
    /**
     * One mesh vertex, 64 bytes. `tangent` (appended last, so a positional aggregate initialiser that stops at
     * `color` stays valid and leaves it zero) is the normal map's tangent space: xyz is the direction of
     * increasing u in the vertex's tangent plane, w (+1 or -1) the handedness: bitangent = cross(normal, tangent.xyz)
     * * w, which points along increasing v (the engine's v-up convention, the glTF normal texture's green-up). A
     * zero tangent means "none": the lit shaders then draw the vertex without normal mapping.
     */
    struct Vertex
    {
        float position[3];
        float normal[3];
        float texCoord[2];
        float color[4];
        float tangent[4];
    };

    static_assert(sizeof(Vertex) == 64, "Vertex is 64 bytes: the OpenGL attribute layout and the importer's ImportedVertex copy depend on it");

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