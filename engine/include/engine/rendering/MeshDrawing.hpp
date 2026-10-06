#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <math/Matrix.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/rendering/GpuCache.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"

// The drawing code every mesh component shares (MeshRenderer, and the built-in shapes CubeRenderer, SphereRenderer
// and QuadRenderer): a Mesh's submeshes, each with its material, through GPU resources shared per renderer in the
// GpuCache. Internal to the engine's renderables; game code uses the components.
namespace N2Engine::Rendering::MeshDrawing
{
    /// The queue a material draws in: Transparent for Blend, Opaque for Opaque and Mask
    [[nodiscard]] RenderQueue QueueFor(const Material &material);

    /// Whether a world matrix mirrors what it draws (the determinant of its upper 3 x 3 is negative, as with an odd
    /// number of negative scales), which turns the winding of every triangle around on screen
    [[nodiscard]] bool IsMirrored(const Math::Matrix<float, 4, 4> &world);

    /**
     * The state one submesh draws with: its queue's state, with culling off for a double-sided material, and
     * otherwise the culled face swapped (Front for Back) when the world matrix mirrors, because both backends tell
     * front faces by their winding on screen.
     */
    [[nodiscard]] Renderer::Common::RenderState StateFor(const Material &material,
                                                         const Renderer::Common::RenderState &queueState, bool mirrored);

    /**
     * One mesh component's GPU resources on one renderer: a share of its mesh's GPU mesh and of each submesh's GPU
     * material, all from the GpuCache (one per renderer and asset, shared by every component), never made or
     * destroyed directly.
     *
     * Like the other renderables, it holds the renderer's lifetime token (IRenderer::GetLifetimeToken): a renderer
     * that was destroyed, or replaced by a new one at the same address, is never called.
     */
    class DrawResources
    {
    public:
        DrawResources() = default;
        /// Releases everything without calling the renderer, which may be gone by now
        ~DrawResources();
        DrawResources(const DrawResources &) = delete;
        DrawResources &operator=(const DrawResources &) = delete;

        /// The renderer resources are acquired on (nullptr when none is bound)
        [[nodiscard]] Renderer::Common::IRenderer *GetRenderer() const { return _renderer; }
        /// Whether resources are bound to this renderer: the same one, still alive
        [[nodiscard]] bool Holds(const Renderer::Common::IRenderer *renderer) const;

        /// Makes `renderer` (not null) the one resources are acquired on, releasing any held on another one (or on
        /// a renderer that no longer exists, without calling it)
        void Bind(Renderer::Common::IRenderer *renderer);

        /// Gives up every share. With callRenderer false (a destructor, when the renderer may be gone) the
        /// renderer is never called; otherwise the last share of a resource destroys it, if its renderer exists.
        void Release(bool callRenderer);

        /**
         * Draws the submeshes of `mesh` whose material belongs in `queue`, on the bound renderer, each with
         * DrawMesh(gpuMesh, world, gpuMaterial, StateFor(...), IndexRange). Submesh i is drawn with materials[i],
         * or with `fallback` when that is missing or null. Every draw's uniforms are set from its material with
         * `tint` (Material::ApplyUniforms), so shared GPU materials always draw with the right values.
         *
         * Acquires (or re-acquires, after a change of mesh, renderer or material version) what it needs. Returns
         * the number of draws: 0 without a bound renderer or a mesh, or when the renderer can't make the mesh.
         * A submesh whose material the renderer can't make (no such shader) is skipped.
         */
        std::size_t Draw(const std::shared_ptr<const Mesh> &mesh,
                         std::span<const std::shared_ptr<Material>> materials,
                         const std::shared_ptr<const Material> &fallback, const Math::Matrix<float, 4, 4> &world,
                         const Common::Color &tint, RenderQueue queue, const Renderer::Common::RenderState &queueState);

        // Introspection, for tests
        [[nodiscard]] const GpuCache::Handle &GetMeshHandle() const { return _mesh; }
        /// The material shares held, one per submesh drawn so far (empty handles for slots not yet drawn)
        [[nodiscard]] std::span<const GpuCache::Handle> GetMaterialHandles() const { return _materials; }

    private:
        /// The material share for slot `index`, (re)acquired for `material`'s current version as needed
        Renderer::Common::IMaterial *EnsureMaterial(std::size_t index, const std::shared_ptr<const Material> &material);

        Renderer::Common::IRenderer *_renderer = nullptr;
        std::weak_ptr<const void> _rendererLifetime; // expired once _renderer is destroyed
        GpuCache::Handle _mesh;
        std::vector<GpuCache::Handle> _materials; // per submesh
        // The last mesh the renderer couldn't make, so it isn't retried every frame (a backend that can't draw)
        std::weak_ptr<const Mesh> _failedMesh;
        std::uint64_t _failedMeshVersion = 0;
    };
}
