#pragma once

#include "assetimport/ModelImporter.hpp"

namespace N2Engine::AssetImport
{
    /**
     * glTF 2.0 (.gltf with data: URIs or external .bin files, and .glb) through cgltf.
     *
     * - The file is checked with cgltf_validate, and every accessor, buffer view, stride and index again here.
     * - Triangle strips and fans become lists; points and lines are skipped with a warning.
     * - Indices may be u8, u16 or u32; attributes may be float or normalised integers; sparse accessors work.
     * - Texture coordinates are flipped (v' = 1 - v) to the engine's v-up convention.
     * - A mesh's primitives share one vertex buffer, one submesh each.
     * - Missing normals are generated flat (the glTF rule).
     * - Tangents: the file's TANGENT (VEC4) is kept; a primitive without one, or whose normals were generated, gets
     *   MikkTSpace tangents from its positions, normals and (flipped) texture coordinates, which may duplicate
     *   vertices where a vertex needs two tangents (ModelImportSettings::generateTangents).
     * - KHR_materials_unlit gives unlit materials; roughness becomes smoothness = 1 - roughness.
     * - Draco and meshopt compression are errors. KHR_texture_transform, other extensions, TEXCOORD_1, cameras,
     *   lights, skins, morph targets and animations are ignored, each with one warning.
     */
    class GltfImporter final : public IModelImporter
    {
    public:
        [[nodiscard]] std::expected<ImportedScene, ModelImportError> Import(
            std::span<const std::uint8_t> bytes, const std::filesystem::path &baseDirectory,
            const ModelImportSettings &settings = {}) const override;

        /// ".gltf" and ".glb"
        [[nodiscard]] bool HandlesExtension(std::string_view extension) const override;

        [[nodiscard]] std::string_view GetName() const override { return "glTF 2.0 (cgltf)"; }
    };
}
