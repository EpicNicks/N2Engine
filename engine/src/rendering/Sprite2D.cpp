#include "engine/rendering/Sprite2D.hpp"

#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "engine/GameObject.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        // The software renderer is the one that cannot blend (SoftwareRenderer::GetRendererName)
        bool CanBlend(const Renderer::Common::IRenderer &renderer)
        {
            return std::string_view(renderer.GetRendererName()) != "Software Rasterizer";
        }

        // What decides the mesh, so a sprite rebuilds it only when one of these changed
        std::string KeyOf(const ShapeDesc2D &d)
        {
            std::string key(reinterpret_cast<const char *>(&d.shape), 1);
            for (const float f : {d.size.x, d.size.y, static_cast<float>(d.sides), d.outline, d.regionOffset.x, d.regionOffset.y,
                                  d.regionSize.x, d.regionSize.y, d.pivot.x, d.pivot.y})
            {
                key.append(reinterpret_cast<const char *>(&f), sizeof f);
            }
            for (const Math::Vector2 &p : d.vertices)
            {
                key.append(reinterpret_cast<const char *>(&p.x), sizeof p.x);
                key.append(reinterpret_cast<const char *>(&p.y), sizeof p.y);
            }
            return key;
        }
    }

    Sprite2D::Sprite2D(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();
        RegisterMember(NAMEOF(_shape), _shape);
        RegisterMember(NAMEOF(_size), _size);
        RegisterMember(NAMEOF(_sides), _sides);
        RegisterMember(NAMEOF(_vertices), _vertices);
        RegisterMember(NAMEOF(_outline), _outline);
        RegisterMember(NAMEOF(_color), _color);
        RegisterAssetRef(NAMEOF(_texture), _texture);
        RegisterMember(NAMEOF(_regionOffset), _regionOffset);
        RegisterMember(NAMEOF(_regionSize), _regionSize);
        RegisterMember(NAMEOF(_flipX), _flipX);
        RegisterMember(NAMEOF(_flipY), _flipY);
        RegisterMember(NAMEOF(_pivot), _pivot);
        RegisterMember(NAMEOF(_sortingOrder), _sortingOrder);
    }

    Sprite2D::~Sprite2D()
    {
        // A sprite freed without OnDestroy (its object never joined a scene) may outlive its renderer
        _resources.Release(false);
    }

    std::string Sprite2D::GetTypeName() const
    {
        return NAMEOF(Sprite2D);
    }

    RenderQueueKey Sprite2D::GetRenderQueue() const
    {
        return {RenderQueue::Transparent, _sortingOrder};
    }

    void Sprite2D::SetTexture(std::shared_ptr<Texture> texture)
    {
        _texture = std::move(texture);
    }

    ShapeDesc2D Sprite2D::Describe() const
    {
        ShapeDesc2D desc;
        desc.shape = _shape;
        desc.size = _size;
        desc.sides = _sides;
        desc.vertices = _vertices;
        desc.outline = _outline;
        desc.regionOffset = _regionOffset;
        desc.regionSize = _regionSize;
        desc.pivot = _pivot;
        return desc;
    }

    std::shared_ptr<Mesh> Sprite2D::GetMesh()
    {
        const ShapeDesc2D desc = Describe();
        std::string key = KeyOf(desc);
        if (key != _meshKey)
        {
            _mesh = GetShapeMesh2D(desc);
            _meshKey = std::move(key);
        }
        return _mesh;
    }

    std::optional<Positionable::Matrix4> Sprite2D::GetModelMatrix() const
    {
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return std::nullopt;
        }
        Positionable::Matrix4 flip{Positionable::Matrix4::identity()};
        flip(0, 0) = _flipX ? -1.0f : 1.0f;
        flip(1, 1) = _flipY ? -1.0f : 1.0f;
        return positionable->GetLocalToWorldMatrix() * flip;
    }

    std::optional<BoundingBox> Sprite2D::GetWorldBounds() const
    {
        const std::optional<Positionable::Matrix4> world = GetModelMatrix();
        if (!world)
        {
            return std::nullopt;
        }
        // GetMesh only caches: it does not change what the sprite is
        const std::shared_ptr<Mesh> mesh = const_cast<Sprite2D *>(this)->GetMesh();
        if (!mesh)
        {
            return std::nullopt;
        }
        return mesh->GetBounds().Transformed(*world);
    }

    const std::shared_ptr<Material> &Sprite2D::EnsureMaterial(const Renderer::Common::IRenderer &renderer)
    {
        if (!_material)
        {
            _material = Material::Create(ShadingModel::Unlit);
            _material->SetAlphaMode(AlphaMode::Blend);
            _material->SetDoubleSided(true);
        }
        if (_material->GetBaseColorTexture() != _texture)
        {
            _material->SetBaseColorTexture(_texture);
        }
        const float discard = (_texture && !CanBlend(renderer)) ? 0.5f : 0.0f;
        if (_material->GetBlendDiscardBelow() != discard)
        {
            _material->SetBlendDiscardBelow(discard);
        }
        return _material;
    }

    void Sprite2D::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
    }

    void Sprite2D::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                 const RenderQueue queue)
    {
        if (!renderer || queue != RenderQueue::Transparent)
        {
            return;
        }
        const std::optional<Positionable::Matrix4> world = GetModelMatrix();
        const std::shared_ptr<Mesh> mesh = world ? GetMesh() : nullptr;
        if (!mesh)
        {
            return;
        }
        const std::shared_ptr<Material> &material = EnsureMaterial(*renderer);
        _resources.Bind(renderer);
        _resources.Draw(mesh, std::span<const std::shared_ptr<Material>>(&material, 1), Material::GetDefaultUnlit(), *world,
                        _color, queue, state);
    }

    void Sprite2D::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            _resources.Bind(renderer);
        }
    }

    void Sprite2D::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || renderer != _resources.GetRenderer())
        {
            return;
        }
        _resources.Release(true);
    }

    void Sprite2D::OnDestroy()
    {
        CleanupRenderResources(_resources.GetRenderer());
    }
}
