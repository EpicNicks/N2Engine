#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assetimport/ModelImporter.hpp"

// Which model importers this build has (#3 P5): glTF (GltfImporter) always, FBX and OBJ (UfbxImporter) only when the
// library was built with N2ENGINE_MODEL_UFBX. The engine asks here instead of naming an importer, so nothing outside
// this library needs the option.
namespace N2Engine::AssetImport
{
    /// Every importer built, glTF first
    [[nodiscard]] std::span<const IModelImporter *const> GetModelImporters();

    /// The importer for a file extension (with the dot, any case: ".FBX"), or nullptr when no built importer reads it
    [[nodiscard]] const IModelImporter *FindModelImporter(std::string_view extension);

    /// Every extension a built importer reads, lower case with the dot (".gltf", ".glb", and with ufbx ".fbx", ".obj")
    [[nodiscard]] std::vector<std::string> GetModelExtensions();

    /// Whether the optional ufbx importer (FBX and OBJ) was built
    [[nodiscard]] bool IsUfbxAvailable();
}
