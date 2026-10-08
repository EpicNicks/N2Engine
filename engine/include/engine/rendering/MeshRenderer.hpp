#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/Camera.hpp" // BoundingBox
#include "engine/IRenderable.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshDrawing.hpp"

namespace N2Engine::Rendering
{
    /**
     * Draws a Mesh at its GameObject's transform, each submesh with its own Material. There is no separate
     * MeshFilter: the mesh reference lives here.
     *
     * - The mesh (`_mesh`, saved as its asset UUID; the built-in meshes have fixed UUIDs) and one material per
     *   submesh (`_materials`, a list of asset UUIDs, null for an empty slot). Slot i draws submesh i; an empty or
     *   missing slot draws with Material::GetDefault() (lit white), and slots past the last submesh are kept but
     *   unused.
     * - Queues: a submesh whose material is Blend draws in the Transparent queue (blended, sorted back to front
     *   by the object's position); the rest draw in the Opaque queue, unblended. A renderer with both kinds draws
     *   in both queues (IRenderable::DrawsInQueue), each submesh once, in its own queue.
     * - Culling: back faces, except a double-sided material draws both; a mirrored world matrix (negative
     *   determinant, e.g. a scale of -1 on one axis) culls front faces instead, so mirrored objects aren't turned
     *   inside out.
     * - GPU resources come from the GpuCache, one per renderer and asset, shared by every MeshRenderer (and
     *   built-in shape) using the same mesh or material; the component only holds shares, released in OnDestroy.
     */
    class MeshRenderer final : public IRenderable
    {
    public:
        explicit MeshRenderer(GameObject &gameObject);
        ~MeshRenderer() override;

        [[nodiscard]] std::string GetTypeName() const override { return "MeshRenderer"; }

        // IRenderable
        /// Opaque when any submesh draws opaque (or there is no mesh), else Transparent; sort key 0
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override;
        /// Whether any submesh's material belongs in `queue`
        [[nodiscard]] bool DrawsInQueue(RenderQueue queue) const override;
        /// The mesh's bounds moved by the object's world transform; nullopt without a mesh or a transform
        [[nodiscard]] std::optional<BoundingBox> GetWorldBounds() const override;
        /// Draws every submesh outside a scene: the opaque ones with RenderState::Opaque(), then the blended ones
        /// with RenderState::Transparent()
        void Render(Renderer::Common::IRenderer *renderer) override;
        /// Draws the submeshes whose material belongs in `queue`
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        /// Resources are acquired on first draw; this only binds the component to the renderer (releasing what it
        /// held on another one)
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        /// Releases the shares this component holds on `renderer`. Does nothing for a renderer it holds nothing on.
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;
        void OnDestroy() override;

        /// The mesh drawn (nullptr draws nothing)
        void SetMesh(std::shared_ptr<Mesh> mesh) { _mesh = std::move(mesh); }
        [[nodiscard]] const std::shared_ptr<Mesh> &GetMesh() const { return _mesh; }

        /// The number of material slots that draw: the mesh's submesh count (0 without a mesh)
        [[nodiscard]] std::size_t GetMaterialCount() const;
        /// Slot `index` (0-based): its material, or nullptr for an empty slot (drawn with Material::GetDefault())
        [[nodiscard]] std::shared_ptr<Material> GetMaterial(std::size_t index) const;
        /// Fills slot `index`; nullptr empties it. The list grows (with empty slots) to reach `index`.
        void SetMaterial(std::size_t index, std::shared_ptr<Material> material);
        /// Every slot as saved, including empty ones and ones past the last submesh
        [[nodiscard]] const std::vector<std::shared_ptr<Material>> &GetMaterials() const { return _materials; }
        void SetMaterials(std::vector<std::shared_ptr<Material>> materials) { _materials = std::move(materials); }
        /// What slot `index` draws with: its material, or the default (lit white) for an empty slot
        [[nodiscard]] std::shared_ptr<const Material> GetEffectiveMaterial(std::size_t index) const;

        /// The mesh's bounds in its own space (before the object's transform); nullopt without a mesh
        [[nodiscard]] std::optional<BoundingBox> GetBounds() const;

        /// The GPU shares, for tests
        [[nodiscard]] const MeshDrawing::DrawResources &GetDrawResources() const { return _resources; }

        static constexpr bool IsSingleton = false;

    private:
        std::shared_ptr<Mesh> _mesh;                    // serialized as "_mesh"
        std::vector<std::shared_ptr<Material>> _materials; // serialized as "_materials"
        MeshDrawing::DrawResources _resources;
    };
}
