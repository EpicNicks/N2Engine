#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "assetimport/ImportedScene.hpp"

// The library-neutral seam between model files and the engine (#3 P3): an importer turns a file's bytes into an
// ImportedScene. glTF 2.0 (GltfImporter, via cgltf) is the one implementation today; an optional FBX/OBJ importer
// (ufbx) can follow behind the same interface.
namespace N2Engine::AssetImport
{
    // ===== Fixed caps (decision C on #3). Files over them are rejected before the memory is allocated. =====

    /// The largest model file (.glb, or .gltf JSON) an importer reads: 512 MiB
    inline constexpr std::size_t kMaxModelFileBytes = std::size_t{512} * 1024 * 1024;
    /// The largest glTF JSON (a .gltf file, or a .glb's JSON chunk): 64 MiB
    inline constexpr std::size_t kMaxModelJsonBytes = std::size_t{64} * 1024 * 1024;
    /// The largest single buffer (a .glb's BIN chunk, a data: URI or an external .bin): 512 MiB
    inline constexpr std::size_t kMaxModelBufferBytes = std::size_t{512} * 1024 * 1024;
    /// The most buffer data one model loads in all: 1 GiB
    inline constexpr std::size_t kMaxModelTotalBufferBytes = std::size_t{1024} * 1024 * 1024;
    /// The largest image file a model references (external or embedded): 64 MiB, encoded
    inline constexpr std::size_t kMaxModelImageBytes = std::size_t{64} * 1024 * 1024;
    /// The most elements an accessor may have (vertices, indices): 16 M
    inline constexpr std::size_t kMaxAccessorElements = std::size_t{1} << 24;
    /// The most vertices or indices one imported mesh may have after triangulation: 64 M
    inline constexpr std::size_t kMaxMeshElements = std::size_t{1} << 26;
    /// How deep a node tree may go
    inline constexpr std::size_t kMaxNodeDepth = 256;

    /// What to do about vertex normals
    enum class NormalGeneration : std::uint8_t
    {
        /// Keep the file's; a primitive without normals gets (0, 0, 1)
        Never,
        /// Generate flat normals for a primitive that has none (glTF's rule: "flat normals")
        IfMissing,
        /// Ignore the file's normals and generate flat ones everywhere
        Always
    };

    struct ModelImportSettings
    {
        /// Multiplies every position and node translation (a model in centimetres takes 0.01). Must be finite
        /// and above 0; anything else is treated as 1.
        float scale = 1.0f;
        /// Import materials and the images they use; off gives every submesh material index -1
        bool importMaterials = true;
        NormalGeneration generateNormals = NormalGeneration::IfMissing;
        /// Join a mesh's primitives that share a material into one submesh each
        bool mergeSubmeshesByMaterial = false;
    };

    enum class ModelImportErrorCode
    {
        /// No bytes at all
        EmptyInput,
        /// Over one of the size caps above
        TooLarge,
        /// Not glTF/GLB, truncated, or JSON the parser rejects
        ParseFailed,
        /// Parsed, but inconsistent (cgltf_validate, or this importer's own checks: an accessor or buffer view
        /// past its data, a bad stride, an index past the last vertex, an unsupported component type, ...)
        InvalidData,
        /// A feature this importer can't read: Draco or meshopt compression
        Unsupported,
        /// A buffer's data couldn't be read: a missing or short .bin, a bad data: URI, or a URI that leaves the
        /// model's folder or names a remote resource
        BufferLoadFailed
    };

    struct ModelImportError
    {
        ModelImportErrorCode code = ModelImportErrorCode::ParseFailed;
        /// A sentence for the log
        std::string message;
    };

    /// A short name for the code ("InvalidData")
    [[nodiscard]] std::string ToString(ModelImportErrorCode code);

    class IModelImporter
    {
    public:
        virtual ~IModelImporter() = default;

        /**
         * Imports a model file held in memory. `baseDirectory` is the folder relative URIs (external buffers and
         * images) are resolved against; every one must stay inside it. With an empty `baseDirectory`, a file
         * that references another file is an error (buffers) or a warning (images). Never throws; malformed
         * input is an error value, never a crash.
         */
        [[nodiscard]] virtual std::expected<ImportedScene, ModelImportError> Import(
            std::span<const std::uint8_t> bytes, const std::filesystem::path &baseDirectory,
            const ModelImportSettings &settings = {}) const = 0;

        /// Whether this importer reads files with this extension (lower case, with the dot: ".glb")
        [[nodiscard]] virtual bool HandlesExtension(std::string_view extension) const = 0;

        /// "glTF 2.0 (cgltf)"
        [[nodiscard]] virtual std::string_view GetName() const = 0;
    };

    /**
     * A relative URI from a model file (already percent-decoded) joined onto `baseDirectory`, or an empty path when
     * it must not be read: a URI with a scheme ("http:", "file:", a drive letter), an absolute path, an empty one,
     * one with a NUL, or one that leads outside `baseDirectory` once normalised ("../x.bin"). Purely lexical.
     */
    [[nodiscard]] std::filesystem::path ResolveModelUri(const std::filesystem::path &baseDirectory,
                                                        std::string_view decodedUri);

    /// RFC 3986 percent-decoding ("my%20mesh.bin" is "my mesh.bin"); a malformed escape is kept as written
    [[nodiscard]] std::string PercentDecode(std::string_view uri);
}
