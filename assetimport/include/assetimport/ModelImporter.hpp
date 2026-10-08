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
    /// The most image bytes one model reads in all (every image, embedded or external): 256 MiB, encoded
    inline constexpr std::size_t kMaxModelTotalImageBytes = std::size_t{256} * 1024 * 1024;
    /// The most elements an accessor may have (vertices, indices): 16 M
    inline constexpr std::size_t kMaxAccessorElements = std::size_t{1} << 24;
    /// The most vertices one model may read in all, counted per primitive (a primitive reusing another's
    /// accessors counts again, so does an accessor without a buffer view, which costs no file bytes) plus the
    /// vertices flat normals add: 8 M
    inline constexpr std::size_t kMaxSceneVertices = std::size_t{1} << 23;
    /// The most indices one model may read and make in all (as read, and as triangle lists): 32 M
    inline constexpr std::size_t kMaxSceneIndices = std::size_t{1} << 25;
    /// The most memory cgltf may allocate while parsing the JSON (its tokens and object arrays): 256 MiB. A small
    /// JSON can declare millions of empty objects, each of which cgltf allocates in full.
    inline constexpr std::size_t kMaxParseMemoryBytes = std::size_t{256} * 1024 * 1024;
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

    /// What to do about vertex tangents (for normal maps)
    enum class TangentGeneration : std::uint8_t
    {
        /// Keep the file's TANGENT; a primitive without any has none (zero tangents: no normal mapping)
        Never,
        /// Generate MikkTSpace tangents for a primitive that has no TANGENT (glTF's rule), and for one whose normals
        /// were generated, since the file's tangents belong to its own normals
        IfMissing,
        /// Ignore the file's tangents and generate everywhere
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
        TangentGeneration generateTangents = TangentGeneration::IfMissing;
        /// Join a mesh's primitives that share a material into one submesh each
        bool mergeSubmeshesByMaterial = false;
    };

    enum class ModelImportErrorCode
    {
        /// No bytes at all
        EmptyInput,
        /// Over one of the size caps above, or out of memory
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
