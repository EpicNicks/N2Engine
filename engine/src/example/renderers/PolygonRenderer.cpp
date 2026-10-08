#include "engine/example/renderers/PolygonRenderer.hpp"

#include <span>
#include <utility>

namespace N2Engine::Example
{
    PolygonRenderer::PolygonRenderer(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();
        RegisterMember(NAMEOF(_color), _color);
        RegisterMember(NAMEOF(_size), _size);
        RegisterAssetRef(NAMEOF(_material), _material);
    }

    PolygonRenderer::~PolygonRenderer()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its GameObject never
        // joined a scene) may outlive its renderer, so the renderer isn't called.
        _resources.Release(false);
    }

    RenderQueueKey PolygonRenderer::GetRenderQueue() const
    {
        if (_material && _material->IsBlended())
        {
            return {RenderQueue::Transparent, 0};
        }
        return {};
    }

    void PolygonRenderer::Render(Renderer::Common::IRenderer *renderer)
    {
        if (GetRenderQueue().queue == RenderQueue::Transparent)
        {
            RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
        }
        else
        {
            RenderInQueue(renderer, Renderer::Common::RenderState::Opaque(), RenderQueue::Opaque);
        }
    }

    void PolygonRenderer::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                        const RenderQueue queue)
    {
        if (!renderer)
        {
            return;
        }
        const std::optional<Positionable::Matrix4> world = GetModelMatrix();
        if (!world)
        {
            return;
        }
        const std::shared_ptr<Rendering::Mesh> mesh = GetMesh();
        if (!mesh)
        {
            return;
        }

        _resources.Bind(renderer);
        _resources.Draw(mesh, std::span<const std::shared_ptr<Rendering::Material>>(&_material, 1),
                        Rendering::Material::GetDefaultUnlit(), *world, _color, queue, state);
    }

    std::optional<Positionable::Matrix4> PolygonRenderer::GetModelMatrix() const
    {
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return std::nullopt;
        }
        // The world transform (with the hierarchy), times the shape's size
        Positionable::Matrix4 scaleMatrix{Positionable::Matrix4::identity()};
        scaleMatrix(0, 0) = _size.x;
        scaleMatrix(1, 1) = _size.y;
        scaleMatrix(2, 2) = _size.z;
        return positionable->GetLocalToWorldMatrix() * scaleMatrix;
    }

    std::optional<BoundingBox> PolygonRenderer::GetWorldBounds() const
    {
        const std::optional<Positionable::Matrix4> world = GetModelMatrix();
        if (!world)
        {
            return std::nullopt;
        }
        // GetMesh may make (and remember) a custom sphere: a cache, not a change to what the shape is
        const std::shared_ptr<Rendering::Mesh> mesh = const_cast<PolygonRenderer *>(this)->GetMesh();
        if (!mesh)
        {
            return std::nullopt;
        }
        return mesh->GetBounds().Transformed(*world);
    }

    void PolygonRenderer::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            _resources.Bind(renderer);
        }
    }

    void PolygonRenderer::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        // Only the renderer the resources belong to releases them
        if (!renderer || renderer != _resources.GetRenderer())
        {
            return;
        }
        _resources.Release(true);
    }

    void PolygonRenderer::OnDestroy()
    {
        CleanupRenderResources(_resources.GetRenderer());
    }
}
