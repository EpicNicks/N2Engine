#pragma once

#include "assetimport/ModelImporter.hpp"

namespace N2Engine::AssetImport
{
    /**
     * FBX (binary and ASCII) and Wavefront OBJ through ufbx (#3 P5). Only built with the CMake option
     * N2ENGINE_MODEL_UFBX; use ModelImporters.hpp to ask whether it is there.
     *
     * - ufbx converts the scene to the engine's space on load: right-handed, y up, metres (FBX files are often in
     *   centimetres or z up; an OBJ has no units or axes, so it is taken as right-handed, y up, in metres).
     *   ModelImportSettings::scale multiplies on top. A left-handed file (ufbx's axes say so, as the
     *   DirectX preset's) is mirrored across x, geometry and nodes together, and its winding reversed to match.
     * - Texture coordinates are kept as written: FBX and OBJ are already v-up, so (unlike glTF) there is no flip.
     * - Polygons are triangulated (ufbx). A mesh's vertices are welded by value: corners whose position, normal,
     *   UV, colour and tangent are all equal share one vertex, whatever indices the file gave their attributes. One submesh per material slot used.
     * - Normals: the file's are kept. generateNormals = IfMissing has ufbx generate smooth normals (from the file's
     *   smoothing groups) for a mesh with none, Always ignores the file's and makes flat ones. Tangents: as for glTF
     *   (the file's, else MikkTSpace for a mesh with a normal-mapped material, see ModelImportSettings).
     * - Materials come from ufbx's PBR view of the file's material (Blender's FBX exporter included): base colour,
     *   opacity (Blend when below 1), roughness, metalness, emission, and the base colour, normal, occlusion and
     *   emission textures. An FBX texture's embedded bytes are used, else the file it names, read from inside the
     *   model's folder (by its relative path, then by its bare file name).
     * - An OBJ's first `mtllib` is read from the model's folder.
     * - Only the first UV set is read (a warning says when there are more). Skins, blend shapes, animation, cameras, lights and non-mesh geometry (NURBS, curves) are ignored with a
     *   warning. Per-instance material overrides are ignored: a mesh uses its own materials.
     */
    class UfbxImporter final : public IModelImporter
    {
    public:
        [[nodiscard]] std::expected<ImportedScene, ModelImportError> Import(
            std::span<const std::uint8_t> bytes, const std::filesystem::path &baseDirectory,
            const ModelImportSettings &settings = {}) const override;

        /// ".fbx" and ".obj"
        [[nodiscard]] bool HandlesExtension(std::string_view extension) const override;

        [[nodiscard]] std::string_view GetName() const override { return "FBX/OBJ (ufbx)"; }
    };
}
