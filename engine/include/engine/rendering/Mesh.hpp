#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <math/UUID.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/Camera.hpp" // BoundingBox
#include "engine/base/Asset.hpp"

namespace N2Engine::Rendering
{
    /**
     * A run of a mesh's indices drawn with one material: `indexCount` indices from `firstIndex` (whole
     * triangles, so both are multiples of 3), and the axis-aligned box around the vertices those indices use,
     * in mesh space. Mesh::Create computes the bounds.
     */
    struct Submesh
    {
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        BoundingBox bounds;
    };

    /// The meshes every renderer has without any file: the shapes CubeRenderer, SphereRenderer and QuadRenderer draw
    enum class BuiltinMesh : std::uint8_t
    {
        Cube,   ///< 1 x 1 x 1, centred, 24 vertices (4 per face, so each face has its own normal)
        Sphere, ///< diameter 1, centred, 16 rings x 32 slices (v runs from 0 at the top pole: see MakeSphere)
        Quad    ///< 1 x 1 on the x/y plane, centred, facing +Z
    };

    /**
     * A triangle mesh asset: CPU vertices and indices, split into one or more submeshes (each drawn with its own
     * material), with bounds. It never holds a GPU handle, so it works headless; each renderer gets one GPU mesh
     * per Mesh through the engine's GPU cache (GpuCache::AcquireMesh), shared by every component drawing it.
     *
     * Conventions: right-handed, y up, front faces wind counter-clockwise, units are metres, and texture
     * coordinates are v-up (v = 0 is the first row of a texture's pixel data, the bottom of an imported image),
     * except the built-in sphere's, whose v = 0 is its top pole (kept from SphereRenderer).
     *
     * Every Mesh has at least one submesh. GetVersion goes up on every change (SetData), so GPU copies are
     * re-uploaded. The built-in meshes (GetBuiltin) have fixed UUIDs, so scene files can name them, and never
     * change.
     */
    class Mesh final : public Base::Asset
    {
    public:
        Mesh() = default;

        /**
         * A mesh made at runtime (procedural geometry). With no submeshes, one submesh covers every index.
         * nullptr, with an error logged, if the data isn't a valid triangle mesh: no vertices or indices, an index
         * count that isn't a multiple of 3, an index past the last vertex, or a submesh that is empty, isn't
         * whole triangles, or runs past the indices. Each submesh's bounds are computed (any given are replaced).
         */
        [[nodiscard]] static std::shared_ptr<Mesh> Create(Renderer::Common::MeshData data,
                                                          std::vector<Submesh> submeshes = {});

        /// The shared built-in mesh (made once, on first use). Never null.
        [[nodiscard]] static std::shared_ptr<Mesh> GetBuiltin(BuiltinMesh which);
        /// The fixed UUID scene files save for a built-in mesh
        [[nodiscard]] static Math::UUID GetBuiltinUUID(BuiltinMesh which);
        /// The built-in mesh with this UUID, or nullptr. Asset references check it before Resources
        /// (RegisterAssetRef), so the built-ins load from scene files without being registered anywhere.
        [[nodiscard]] static std::shared_ptr<Mesh> FindBuiltin(const Math::UUID &uuid);
        /// "Cube", "Sphere" or "Quad"
        [[nodiscard]] static std::string_view GetBuiltinName(BuiltinMesh which);
        /// The built-in mesh named "Cube", "Sphere" or "Quad" (exactly), or nullopt
        [[nodiscard]] static std::optional<BuiltinMesh> ParseBuiltinName(std::string_view name);

        /// The built-in shapes' geometry: the vertices exactly as CubeRenderer, SphereRenderer and QuadRenderer always
        /// made them (white vertex colour), with every triangle counter-clockwise seen from outside (front faces).
        /// The cube and quad have v-up texture coordinates.
        [[nodiscard]] static Renderer::Common::MeshData MakeCube();
        /// A UV sphere of diameter 1: (latitudeSegments + 1) x (longitudeSegments + 1) vertices, rings from the top
        /// (+y) to the bottom, u going around from +x towards +z. Its v is 0 at the top pole and 1 at the bottom, the
        /// reverse of the engine's v-up convention (kept from SphereRenderer, so a texture shows upside down on it).
        /// Triangles wind counter-clockwise from outside (SphereRenderer's wound clockwise, inside out).
        [[nodiscard]] static Renderer::Common::MeshData MakeSphere(std::uint32_t latitudeSegments = 16,
                                                                   std::uint32_t longitudeSegments = 32);
        [[nodiscard]] static Renderer::Common::MeshData MakeQuad();

        /// The box around the vertices `indices` use (around every vertex when `indices` is empty). A zero box
        /// at the origin when there are none.
        [[nodiscard]] static BoundingBox ComputeBounds(std::span<const Renderer::Common::Vertex> vertices,
                                                       std::span<const std::uint32_t> indices = {});

        /**
         * Replaces the geometry (as Create checks it) and bumps the version, so every renderer re-uploads it on its
         * next draw. False, changing nothing, for invalid data or a built-in mesh (they are shared and fixed).
         */
        bool SetData(Renderer::Common::MeshData data, std::vector<Submesh> submeshes = {});

        /// What GetResourceType() returns, for code that needs it without an instance (asset metadata, a field's asset type)
        static constexpr std::string_view ResourceTypeName = "Mesh";
        [[nodiscard]] std::string GetResourceType() const override { return std::string(ResourceTypeName); }

        [[nodiscard]] const std::vector<Renderer::Common::Vertex> &GetVertices() const { return _data.vertices; }
        [[nodiscard]] const std::vector<std::uint32_t> &GetIndices() const { return _data.indices; }
        /// The vertices and indices, as IRenderer::CreateMesh takes them
        [[nodiscard]] const Renderer::Common::MeshData &GetMeshData() const { return _data; }
        /// At least one for a mesh made by Create or GetBuiltin (none for a default-constructed one)
        [[nodiscard]] std::span<const Submesh> GetSubmeshes() const { return _submeshes; }
        [[nodiscard]] std::size_t GetSubmeshCount() const { return _submeshes.size(); }
        /// The box around every vertex, in mesh space
        [[nodiscard]] const BoundingBox &GetBounds() const { return _bounds; }
        /// Goes up by one on every change; 1 once made
        [[nodiscard]] std::uint64_t GetVersion() const { return _version; }
        [[nodiscard]] bool IsBuiltin() const { return _builtin; }

        /**
         * A CPU-side structure derived from the geometry (the picking BVH), kept with the mesh so it is built once
         * and dropped with the mesh. nullptr when none was set for the current version: SetData makes any earlier one
         * stale. Not thread safe (picking is on the main thread); const because it is a cache, not part of the asset.
         */
        [[nodiscard]] std::shared_ptr<const void> GetCpuCache() const
        {
            return _cpuCacheVersion == _version ? _cpuCache : nullptr;
        }
        /// Keeps `cache` as the CPU cache of the current version
        void SetCpuCache(std::shared_ptr<const void> cache) const
        {
            _cpuCache = std::move(cache);
            _cpuCacheVersion = _version;
        }
        /// A name for messages: the built-in's name, else the resource path, else "mesh"
        [[nodiscard]] std::string GetDebugName() const;

    private:
        /// Checks data and submeshes (filling in a default submesh and every submesh's bounds); the error when invalid
        static std::optional<std::string> Validate(const Renderer::Common::MeshData &data, std::vector<Submesh> &submeshes);

        Renderer::Common::MeshData _data;
        std::vector<Submesh> _submeshes;
        BoundingBox _bounds;
        std::uint64_t _version = 0;
        mutable std::shared_ptr<const void> _cpuCache;
        mutable std::uint64_t _cpuCacheVersion = 0;
        bool _builtin = false;
        std::string _builtinName;
    };
}
