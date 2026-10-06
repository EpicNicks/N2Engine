#include "engine/rendering/MeshRenderer.hpp"

#include <utility>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Rendering
{
    MeshRenderer::MeshRenderer(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();
        RegisterAssetRef(NAMEOF(_mesh), _mesh);
        RegisterAssetRefList(NAMEOF(_materials), _materials);
    }

    MeshRenderer::~MeshRenderer()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its GameObject never
        // joined a scene) may outlive its renderer, so the renderer isn't called.
        _resources.Release(false);
    }

    RenderQueueKey MeshRenderer::GetRenderQueue() const
    {
        if (!DrawsInQueue(RenderQueue::Opaque) && DrawsInQueue(RenderQueue::Transparent))
        {
            return {RenderQueue::Transparent, 0};
        }
        return {};
    }

    bool MeshRenderer::DrawsInQueue(const RenderQueue queue) const
    {
        if (!_mesh)
        {
            return false;
        }
        for (std::size_t i = 0; i < _mesh->GetSubmeshCount(); ++i)
        {
            if (MeshDrawing::QueueFor(*GetEffectiveMaterial(i)) == queue)
            {
                return true;
            }
        }
        return false;
    }

    void MeshRenderer::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Opaque(), RenderQueue::Opaque);
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
    }

    void MeshRenderer::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                     const RenderQueue queue)
    {
        if (!renderer || !_mesh)
        {
            return;
        }
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return;
        }
        _resources.Bind(renderer);
        _resources.Draw(_mesh, _materials, Material::GetDefault(), positionable->GetLocalToWorldMatrix(),
                        Common::Color::White, queue, state);
    }

    void MeshRenderer::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            _resources.Bind(renderer);
        }
    }

    void MeshRenderer::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || renderer != _resources.GetRenderer())
        {
            return;
        }
        _resources.Release(true);
    }

    void MeshRenderer::OnDestroy()
    {
        CleanupRenderResources(_resources.GetRenderer());
    }

    std::size_t MeshRenderer::GetMaterialCount() const
    {
        return _mesh ? _mesh->GetSubmeshCount() : 0;
    }

    std::shared_ptr<Material> MeshRenderer::GetMaterial(const std::size_t index) const
    {
        return index < _materials.size() ? _materials[index] : nullptr;
    }

    void MeshRenderer::SetMaterial(const std::size_t index, std::shared_ptr<Material> material)
    {
        if (index >= _materials.size())
        {
            if (!material)
            {
                return; // already empty
            }
            _materials.resize(index + 1);
        }
        _materials[index] = std::move(material);
    }

    std::shared_ptr<const Material> MeshRenderer::GetEffectiveMaterial(const std::size_t index) const
    {
        if (index < _materials.size() && _materials[index])
        {
            return _materials[index];
        }
        return Material::GetDefault();
    }

    std::optional<BoundingBox> MeshRenderer::GetBounds() const
    {
        if (!_mesh)
        {
            return std::nullopt;
        }
        return _mesh->GetBounds();
    }
}
