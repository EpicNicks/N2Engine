#include "engine/rendering/MeshRenderer2D.hpp"

#include "engine/GameObject.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    MeshRenderer2D::MeshRenderer2D(GameObject &gameObject) : GeometryRenderer2D(gameObject)
    {
        RegisterAssetRef(NAMEOF(_mesh), _mesh);
        RegisterMember(NAMEOF(_color), _color);
        RegisterAssetRef(NAMEOF(_texture), _texture);
    }

    std::string MeshRenderer2D::GetTypeName() const
    {
        return NAMEOF(MeshRenderer2D);
    }

    std::optional<BoundingBox> MeshRenderer2D::GetWorldBounds() const
    {
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable || !_mesh)
        {
            return std::nullopt;
        }
        return _mesh->GetBounds().Transformed(positionable->GetLocalToWorldMatrix());
    }

    void MeshRenderer2D::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                       const RenderQueue queue)
    {
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!renderer || queue != RenderQueue::Transparent || !positionable || !_mesh)
        {
            return;
        }
        _draw.Draw(renderer, _mesh, positionable->GetLocalToWorldMatrix(), _blendMode, _texture, _color, queue, state);
    }
}
