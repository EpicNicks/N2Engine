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
        RegisterMember(NAMEOF(_blendMode), _blendMode);
        RegisterAssetRef(NAMEOF(_texture), _texture);
        RegisterMember(NAMEOF(_regionOffset), _regionOffset);
        RegisterMember(NAMEOF(_regionSize), _regionSize);
        RegisterMember(NAMEOF(_flipX), _flipX);
        RegisterMember(NAMEOF(_flipY), _flipY);
        RegisterMember(NAMEOF(_pivot), _pivot);
        RegisterMember(NAMEOF(_sortingOrder), _sortingOrder);
        // A user shader and its values, only when one is set (stage 9g): a sprite without one saves what it always did
        RegisterOptionalState("shaderState", [this] { return _shaderSlot.Save(); }, [this](const nlohmann::json &json) { _shaderSlot.Load(json); });
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

    namespace
    {
        BlendMode MaterialBlendFor(const SpriteBlend blend)
        {
            switch (blend)
            {
            case SpriteBlend::Additive: return BlendMode::Additive;
            case SpriteBlend::Multiply: return BlendMode::Multiply;
            case SpriteBlend::Alpha: break;
            }
            return BlendMode::Alpha;
        }

        /// Gives `material` the sprite's colour effects, each only when it changed (a change bumps the material's version)
        void SyncColorEffects(Material &material, const float saturation, const Common::Color &flash)
        {
            // A scene file can hold any value, which the material clamps, so compare with what it would store
            if (material.GetSaturation() != Renderer::Common::ColorEffects::ClampSaturation(saturation))
            {
                material.SetSaturation(saturation);
            }
            // Component by component: Color's == allows an epsilon, which would skip a small step of a rainbow
            const Common::Color clamped = Renderer::Common::ColorEffects::ClampFlash(flash);
            const Common::Color &current = material.GetFlash();
            if (current.r != clamped.r || current.g != clamped.g || current.b != clamped.b || current.a != clamped.a)
            {
                material.SetFlash(clamped);
            }
        }
    }

    const std::shared_ptr<Material> &Sprite2D::EnsureMaterial(const Renderer::Common::IRenderer &renderer)
    {
        // Sprites blend on every renderer that blends opted-in draws (the software one too, issue #445), so the cut-out that stands in
        // for blending is only for a renderer that blends nothing (Renderer::Common::BlendCutOutBelow, shared with the UI Image)
        const bool blends = renderer.SupportsBlending() || renderer.SupportsOptInBlending();
        const float discard = Renderer::Common::BlendCutOutBelow(_texture != nullptr, blends, _color.a);
        if (_shaderSlot.HasShader())
        {
            // A user shader: the slot's own Material, with the sprite's texture and the cut-out. The built-in path below is not touched
            _shaderSlot.Sync(_texture, discard);
            Material &material = *_shaderSlot.GetMaterial();
            // The colour effects reach a shader that declares u_Saturation and u_Flash, and the built-in fallback where it cannot draw
            SyncColorEffects(material, _saturation, _flash);
            // The shader's own blend hint wins (a premultiplied shader needs premultiplied blending); without one, the sprite's mode
            const std::shared_ptr<Shader> &shader = _shaderSlot.GetShader();
            if (!shader->GetStateHints().blend && material.GetBlendMode() != MaterialBlendFor(_blendMode))
            {
                material.SetBlendMode(MaterialBlendFor(_blendMode));
            }
            return _shaderSlot.GetMaterial();
        }
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
        if (_material->GetBlendMode() != MaterialBlendFor(_blendMode))
        {
            _material->SetBlendMode(MaterialBlendFor(_blendMode));
        }
        // The test is on the final alpha, the texture's times the colour's: half the colour's alpha cuts out what is under half
        // opaque in the texture, so fading a sprite does not make it vanish, and a fully faded one draws nothing
        if (_material->GetBlendDiscardBelow() != discard)
        {
            _material->SetBlendDiscardBelow(discard);
        }
        // The colour effects reach the GPU as uniforms, so a change costs nothing; set only when changed
        SyncColorEffects(*_material, _saturation, _flash);
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
        // Blended on the software renderer too (issue #445): the draw opts in, and MeshDrawing gives it the material's blend mode
        Renderer::Common::RenderState spriteState = state;
        spriteState.softwareBlend = true;
        _resources.Draw(mesh, std::span<const std::shared_ptr<Material>>(&material, 1), Material::GetDefaultUnlit(), *world,
                        _color, queue, spriteState);
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
