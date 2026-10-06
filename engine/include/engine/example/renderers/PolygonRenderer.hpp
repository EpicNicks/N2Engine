#pragma once

#include <memory>
#include <utility>

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>

#include "engine/Component.hpp"
#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/rendering/Material.hpp"
#include "engine/rendering/Mesh.hpp"
#include "engine/rendering/MeshDrawing.hpp"

#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Example
{
    /**
     * The base of the built-in shapes (CubeRenderer, SphereRenderer, QuadRenderer): a built-in mesh drawn at the
     * object's transform, scaled by its size, through the same drawing code as Rendering::MeshRenderer.
     *
     * - Unlit, in the shape's colour: with no material it draws with Material::GetDefaultUnlit() (unlit white)
     *   tinted by `_color`. An optional `_material` (a Material asset) replaces it, still tinted by the colour, so
     *   a shape can be lit or textured; its alpha mode picks the queue as for MeshRenderer.
     * - Opaque materials (the default) draw in the Opaque queue without blending: a translucent colour no longer
     *   blends (it did before #3 P2). Give the shape a Blend material to draw it see-through.
     * - The GPU mesh and material come from the GpuCache: every shape of a kind on a renderer shares one GPU mesh,
     *   and every shape without a material shares one GPU material, the colour being set per draw.
     *
     * Serialized: `_color`, `_size` and `_material` (asset UUID or null; scenes saved before it load with none).
     */
    class PolygonRenderer : public IRenderable
    {
    public:
        ~PolygonRenderer() override;

        // IRenderable
        /// The material's queue: Transparent for a Blend material, else Opaque (sort key 0)
        [[nodiscard]] RenderQueueKey GetRenderQueue() const override;
        /// Draws in its queue's state outside a scene
        void Render(Renderer::Common::IRenderer *renderer) override;
        void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                           RenderQueue queue) override;
        /// Resources are acquired on first draw; this only binds the component to the renderer (releasing what it
        /// held on another one)
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        /// Releases the shares this component holds on `renderer`. Does nothing for a renderer it holds nothing on.
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;
        void OnDestroy() override;

        // Common properties
        void SetColor(const Common::Color &color) { _color = color; }
        [[nodiscard]] const Common::Color &GetColor() const { return _color; }

        void SetSize(const Math::Vector3 &size) { _size = size; }
        [[nodiscard]] const Math::Vector3 &GetSize() const { return _size; }

        /// The material drawn with (tinted by the colour); nullptr draws unlit in the colour
        void SetMaterial(std::shared_ptr<Rendering::Material> material) { _material = std::move(material); }
        [[nodiscard]] const std::shared_ptr<Rendering::Material> &GetMaterial() const { return _material; }

        /// The mesh this shape draws (a built-in mesh, or a sphere of other subdivisions; nullptr if it can't be made)
        [[nodiscard]] virtual std::shared_ptr<Rendering::Mesh> GetMesh() = 0;

        /// The GPU shares, for tests
        [[nodiscard]] const Rendering::MeshDrawing::DrawResources &GetDrawResources() const { return _resources; }

        static constexpr bool IsSingleton = false;

    protected:
        explicit PolygonRenderer(GameObject &gameObject);

        // Rendering properties (common to all polygon renderers)
        Common::Color _color{Common::Color::White};
        Math::Vector3 _size{Math::Vector3::One};
        std::shared_ptr<Rendering::Material> _material;

    private:
        Rendering::MeshDrawing::DrawResources _resources;
    };
}
