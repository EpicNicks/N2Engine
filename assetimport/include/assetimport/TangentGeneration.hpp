#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "assetimport/ImportedScene.hpp"

// Tangent space generation for normal maps (#3 P4b): MikkTSpace, the tangent space glTF specifies for a normal map
// whose file gives no tangents and that Blender and most baking tools use. CPU only; the vendored MikkTSpace stays
// private to the library.
namespace N2Engine::AssetImport
{
    /// Which vertices GenerateTangents writes
    enum class TangentSelect : std::uint8_t
    {
        /// Only those with no usable tangent (xyz zero or not finite): a file's own tangents are kept
        Missing,
        /// Every vertex, replacing what it had
        All
    };

    struct TangentResult
    {
        /// False when MikkTSpace could not run (out of memory) or the vertex limit would be passed; the mesh is then
        /// unchanged
        bool ok = true;
        /// Vertices appended because a vertex's triangles asked for different tangents (a mirrored UV island, or a
        /// triangle MikkTSpace welded apart): the triangle takes a copy
        std::size_t verticesAdded = 0;
        /// Vertices given an arbitrary tangent perpendicular to their normal (w = +1) because MikkTSpace gave
        /// them none that is usable (degenerate UVs) or no triangle reached them
        std::size_t fallbackVertices = 0;
        /// Vertices given a tangent in all, the appended ones included
        std::size_t verticesWritten = 0;
    };

    /**
     * Generates MikkTSpace tangents for a triangle-list mesh, from its positions, normals and texture
     * coordinates. The texture coordinates are v-up (the engine's convention, see ImportedScene.hpp), so
     * the bitangent cross(normal, tangent.xyz) * tangent.w points along increasing v: the green-up convention of glTF
     * normal maps.
     *
     * `tangent.xyz` is a unit vector perpendicular to the vertex normal, `tangent.w` is +1 or -1. A vertex whose
     * triangles need different tangents is duplicated for the triangles that differ: those indices point at the
     * copy (the index count and every submesh's range are unchanged, vertices may be appended). Never more than
     * `maxVertices` vertices result.
     *
     * Vertices not selected keep their tangent and are never moved. Only whole triangles are read (a trailing
     * partial one is ignored). Indices past the last vertex make it fail with ok = false.
     */
    [[nodiscard]] TangentResult GenerateTangents(std::vector<ImportedVertex> &vertices, std::vector<std::uint32_t> &indices,
                                                 TangentSelect select = TangentSelect::Missing,
                                                 std::size_t maxVertices = std::size_t{1} << 26);
}
