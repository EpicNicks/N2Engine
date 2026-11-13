#include "engine/rendering/Sprite2D.hpp"

#include <algorithm>
#include <span>
#include <utility>

#include <renderer/common/BlendCutOut.hpp>

#include "engine/GameObject.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"

namespace N2Engine::Rendering
{
    Sprite2D::Sprite2D(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();
        RegisterMember(NAMEOF(_shape), _shape);
        RegisterMember(NAMEOF(_size), _size);
        RegisterMember(NAMEOF(_sides), _sides);
        RegisterMember(NAMEOF(_vertices), _vertices);
        RegisterMember(NAMEOF(_outline), _outline);
        RegisterMember(NAMEOF(_color), _color);
        RegisterMember(NAMEOF(_saturation), _saturation).Range(0.0, Renderer::Common::ColorEffects::MaxSaturation);
        RegisterMember(NAMEOF(_flash), _flash);
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
        const auto same = [](const Math::Vector2 &a, const Math::Vector2 &b) { return a.x == b.x && a.y == b.y; };
        const ShapeDesc2D &last = _meshDesc;
        const bool unchanged = _meshMade && last.shape == _shape && same(last.size, _size) && last.sides == _sides &&
                               std::ranges::equal(last.vertices, _vertices, same) && last.outline == _outline &&
                               same(last.regionOffset, _regionOffset) && same(last.regionSize, _regionSize) && same(last.pivot, _pivot);
        if (!unchanged)
        {
            _meshDesc = Describe();
            _mesh = GetShapeMesh2D(_meshDesc);
            _meshMade = true;
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
        // The test is on the final alpha, the texture's times the colour's: half the colour's alpha cuts out what is under half
        // opaque in the texture, so fading a sprite does not make it vanish, and a fully faded one draws nothing
        // (Renderer::Common::BlendCutOutBelow, shared with the UI Image)
        const float discard = Renderer::Common::BlendCutOutBelow(_texture != nullptr, renderer.SupportsBlending(), _color.a);
        if (_material->GetBlendDiscardBelow() != discard)
        {
            _material->SetBlendDiscardBelow(discard);
        }
        // The colour effects reach the GPU as uniforms, so a change costs nothing; set only when changed, as above (a scene
        // file can hold any value, which the material clamps, so compare with what it would store)
        if (_material->GetSaturation() != Renderer::Common::ColorEffects::ClampSaturation(_saturation))
        {
            _material->SetSaturation(_saturation);
        }
        // Component by component: Color's == allows an epsilon, which would skip a small step of a rainbow
        const Common::Color flash = Renderer::Common::ColorEffects::ClampFlash(_flash);
        const Common::Color &current = _material->GetFlash();
        if (current.r != flash.r || current.g != flash.g || current.b != flash.b || current.a != flash.a)
        {
            _material->SetFlash(flash);
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
