#include "engine/ui/Image.hpp"

#include <utility>

#include <renderer/common/RenderTypes.hpp>

namespace N2Engine::UI
{
    Image::Image(GameObject &gameObject) : UIGraphic(gameObject)
    {
        RegisterAssetRef("sprite", _sprite);
    }

    void Image::SetSprite(std::shared_ptr<Rendering::Texture> sprite)
    {
        if (_spriteTexture && _spriteTexture.GetSource() != sprite.get())
        {
            // The renderer is only called if it still exists
            _spriteTexture.Release(true);
        }
        _sprite = std::move(sprite);
    }

    Renderer::Common::ITexture *Image::EnsureSpriteTexture(Renderer::Common::IRenderer *renderer)
    {
        if (!_sprite)
        {
            // Cleared, possibly by deserializing (which sets the field directly): give up any share now
            _spriteTexture.Release(true);
            return nullptr;
        }
        if (!_spriteTexture.Holds(renderer) || _spriteTexture.GetSource() != _sprite.get())
        {
            // A renderer that couldn't create this sprite's texture (Vulkan until #43) isn't asked every frame
            const std::weak_ptr<const void> lifetime = renderer->GetLifetimeToken();
            // Compared by owner, not address: a new sprite or renderer at a freed one's address is a new one
            const bool sameRenderer = !_failedRenderer.owner_before(lifetime) && !lifetime.owner_before(_failedRenderer);
            const bool sameSprite = !_failedSprite.owner_before(_sprite) && !_sprite.owner_before(_failedSprite);
            if (sameSprite && sameRenderer && !_failedSprite.expired() && !_failedRenderer.expired())
            {
                return nullptr;
            }
            // None acquired on this renderer yet, or the sprite changed (deserializing sets the field directly)
            _spriteTexture.Release(true);
            _spriteTexture = Rendering::GpuCache::AcquireTexture(*renderer, _sprite);
            if (_spriteTexture.GetTexture())
            {
                _failedSprite.reset();
                _failedRenderer.reset();
            }
            else
            {
                _failedSprite = _sprite;
                _failedRenderer = lifetime;
            }
        }
        return _spriteTexture.GetTexture();
    }

    Math::Matrix<float, 4, 4> Image::ModelMatrixFor(const Rect &rect)
    {
        // Scale the unit square to the rect's size, then move its corner to the rect's corner
        Math::Matrix<float, 4, 4> model; // zero-filled
        model(0, 0) = rect.width;
        model(1, 1) = rect.height;
        model(2, 2) = 1.0f;
        model(3, 3) = 1.0f;
        model(0, 3) = rect.x;
        model(1, 3) = rect.y;
        return model;
    }

    bool Image::EnsureResources(Renderer::Common::IRenderer *renderer)
    {
        if (_mesh && _material)
        {
            return true;
        }
        _renderer = renderer;
        _rendererLifetime = renderer->GetLifetimeToken();

        if (!_mesh)
        {
            // The unit square, counter-clockwise seen from +z (front-facing in canvas space, which is y up).
            // The UI pass draws with culling off anyway.
            Renderer::Common::MeshData quad;
            quad.vertices = {
                {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
                {{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
            };
            quad.indices = {0, 1, 2, 0, 2, 3};
            _mesh = renderer->CreateMesh(quad);
        }
        if (!_material)
        {
            // The renderer owns its standard shader; the Image never destroys it
            if (Renderer::Common::IShader *shader = renderer->GetStandardUnlitShader())
            {
                _material = renderer->CreateMaterial(shader, nullptr);
            }
        }
        return _mesh && _material;
    }

    void Image::ReleaseResources()
    {
        // A destroyed renderer freed them itself, and its address may now be another renderer's
        if (_renderer && !_rendererLifetime.expired())
        {
            if (_mesh)
            {
                _renderer->DestroyMesh(_mesh);
            }
            if (_material)
            {
                _renderer->DestroyMaterial(_material);
            }
        }
        // The sprite's share of its texture: destroyed with the last share, and only on a renderer that exists
        _spriteTexture.Release(true);
        _mesh = nullptr;
        _material = nullptr;
        _renderer = nullptr;
        _rendererLifetime.reset();
    }

    void Image::RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                         const Renderer::Common::RenderState &state)
    {
        if (!renderer || !rect.HasArea())
        {
            return;
        }
        if (_renderer && (_renderer != renderer || _rendererLifetime.expired()))
        {
            // Drawn by another renderer than the one that made the quad, or by a new renderer at the old one's
            // address (the window was re-created): start again with this one. The old resources are destroyed
            // only if their renderer still exists.
            ReleaseResources();
        }
        if (!EnsureResources(renderer))
        {
            return;
        }

        // A raw texture wins over the sprite
        if (_texture)
        {
            _spriteTexture.Release(true); // the raw texture wins: don't hold a sprite share it hides
        }
        Renderer::Common::ITexture *texture = _texture ? _texture : EnsureSpriteTexture(renderer);
        _material->SetTexture(texture);
        _material->SetInt("uHasTexture", texture != nullptr ? 1 : 0);
        const Common::Color color = GetDrawColor(); // the colour with a Button's tint, if any
        _material->SetColor("uAlbedo", color.r, color.g, color.b, color.a);

        const Math::Matrix<float, 4, 4> model = ModelMatrixFor(rect);
        renderer->DrawMesh(_mesh, model.Data(), _material, state);
    }

    void Image::OnDestroy()
    {
        ReleaseResources();
    }
}
