#include "engine/ui/Image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <renderer/common/BlendCutOut.hpp>
#include <renderer/common/RenderTypes.hpp>

#include "engine/rendering/MeshDrawing.hpp"

namespace N2Engine::UI
{
    Image::Image(GameObject &gameObject) : UIGraphic(gameObject)
    {
        RegisterAssetRef("sprite", _sprite);
        RegisterMember("saturation", _saturation).Range(0.0, Renderer::Common::ColorEffects::MaxSaturation);
        RegisterMember("flash", _flash);
        RegisterMember("sliceBorder", _sliceBorder).Tooltip("9-slice borders in texture pixels: x left, y bottom, z right, w top");
        RegisterMember("sliceScale", _sliceScale).Tooltip("Canvas units per border pixel");
        RegisterMember("fillCenter", _fillCenter);
        // A user shader and its values, only when one is set (stage 9g): an image without one saves what it always did
        RegisterOptionalState("shaderState", [this] { return _shaderSlot.Save(); }, [this](const nlohmann::json &json) { _shaderSlot.Load(json); });
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
            // A renderer that couldn't create this sprite's texture (for example one over the device's size
            // limit) isn't asked every frame
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

    std::optional<Image::Quad> Image::ClipQuad(const Quad &quad, const Rect &clip)
    {
        const Rect visible = Rect::Intersect(quad.position, clip);
        if (!visible.HasArea() || !quad.position.HasArea())
        {
            return std::nullopt;
        }
        // The same share of the texture as of the rect, on each side
        const float uPerX = quad.uv.width / quad.position.width;
        const float vPerY = quad.uv.height / quad.position.height;
        Quad result;
        result.position = visible;
        result.uv = Rect{quad.uv.x + (visible.x - quad.position.x) * uPerX, quad.uv.y + (visible.y - quad.position.y) * vPerY,
                         visible.width * uPerX, visible.height * vPerY};
        return result;
    }

    bool Image::IsSliced() const
    {
        return _sliceBorder.x > 0.0f || _sliceBorder.y > 0.0f || _sliceBorder.z > 0.0f || _sliceBorder.w > 0.0f;
    }

    std::vector<Image::Quad> Image::SliceQuads(const Rect &rect, const Math::Vector2 &textureSize) const
    {
        std::vector<Quad> quads;
        if (!(textureSize.x > 0.0f) || !(textureSize.y > 0.0f))
        {
            return quads;
        }
        auto border = [](const float value) { return std::isfinite(value) && value > 0.0f ? value : 0.0f; };
        const float scale = std::isfinite(_sliceScale) && _sliceScale > 0.0f ? _sliceScale : 1.0f;
        // Texture pixels, at most the whole texture on each axis
        const float leftPx = border(_sliceBorder.x), bottomPx = border(_sliceBorder.y);
        const float rightPx = border(_sliceBorder.z), topPx = border(_sliceBorder.w);
        // Canvas units, shrunk in proportion where two borders don't fit the rect
        float left = leftPx * scale, right = rightPx * scale, bottom = bottomPx * scale, top = topPx * scale;
        if (left + right > rect.width && left + right > 0.0f)
        {
            const float fit = rect.width / (left + right);
            left *= fit;
            right *= fit;
        }
        if (bottom + top > rect.height && bottom + top > 0.0f)
        {
            const float fit = rect.height / (bottom + top);
            bottom *= fit;
            top *= fit;
        }
        const float xs[4] = {rect.x, rect.x + left, rect.XMax() - right, rect.XMax()};
        const float ys[4] = {rect.y, rect.y + bottom, rect.YMax() - top, rect.YMax()};
        const float us[4] = {0.0f, std::min(leftPx / textureSize.x, 1.0f), std::max(1.0f - rightPx / textureSize.x, 0.0f), 1.0f};
        const float vs[4] = {0.0f, std::min(bottomPx / textureSize.y, 1.0f), std::max(1.0f - topPx / textureSize.y, 0.0f), 1.0f};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                if (row == 1 && column == 1 && !_fillCenter)
                {
                    continue;
                }
                const Rect position{xs[column], ys[row], xs[column + 1] - xs[column], ys[row + 1] - ys[row]};
                if (!position.HasArea())
                {
                    continue;
                }
                quads.push_back(Quad{position, Rect{us[column], vs[row], us[column + 1] - us[column], vs[row + 1] - vs[row]}});
            }
        }
        return quads;
    }

    std::vector<Image::Quad> Image::BuildQuads(const Rect &rect, const std::optional<Rect> &clip,
                                               const Math::Vector2 &textureSize) const
    {
        std::vector<Quad> quads;
        const bool sliced = IsSliced() && textureSize.x > 0.0f && textureSize.y > 0.0f;
        if (!sliced)
        {
            if (!clip || clip->ContainsRect(rect))
            {
                return quads; // the plain unit quad
            }
            quads.push_back(Quad{rect});
        }
        else
        {
            quads = SliceQuads(rect, textureSize);
        }
        if (clip && !clip->ContainsRect(rect))
        {
            std::vector<Quad> clipped;
            for (const Quad &quad : quads)
            {
                if (const std::optional<Quad> cut = ClipQuad(quad, *clip))
                {
                    clipped.push_back(*cut);
                }
            }
            quads = std::move(clipped);
        }
        if (quads.empty())
        {
            quads.push_back(Quad{Rect{rect.x, rect.y, 0.0f, 0.0f}}); // nothing left: drawn as nothing
        }
        return quads;
    }

    Renderer::Common::IMesh *Image::EnsureQuadMesh(Renderer::Common::IRenderer *renderer, const std::vector<Quad> &quads,
                                                   const Math::Vector2 &origin)
    {
        // Relative to the rect's corner, so moving the rect alone changes only the model matrix
        std::vector<Quad> local = quads;
        for (Quad &quad : local)
        {
            quad.position.x -= origin.x;
            quad.position.y -= origin.y;
        }
        if (_quadMesh && local == _quadMeshQuads)
        {
            return _quadMesh;
        }
        Renderer::Common::MeshData data;
        data.vertices.reserve(local.size() * 4);
        data.indices.reserve(local.size() * 6);
        for (const Quad &quad : local)
        {
            const auto base = static_cast<std::uint32_t>(data.vertices.size());
            const Rect &p = quad.position;
            const Rect &t = quad.uv;
            // Counter-clockwise seen from +z, as the unit quad
            data.vertices.push_back({{p.XMin(), p.YMin(), 0.0f}, {0.0f, 0.0f, 1.0f}, {t.XMin(), t.YMin()}, {1.0f, 1.0f, 1.0f, 1.0f}});
            data.vertices.push_back({{p.XMax(), p.YMin(), 0.0f}, {0.0f, 0.0f, 1.0f}, {t.XMax(), t.YMin()}, {1.0f, 1.0f, 1.0f, 1.0f}});
            data.vertices.push_back({{p.XMax(), p.YMax(), 0.0f}, {0.0f, 0.0f, 1.0f}, {t.XMax(), t.YMax()}, {1.0f, 1.0f, 1.0f, 1.0f}});
            data.vertices.push_back({{p.XMin(), p.YMax(), 0.0f}, {0.0f, 0.0f, 1.0f}, {t.XMin(), t.YMax()}, {1.0f, 1.0f, 1.0f, 1.0f}});
            data.indices.insert(data.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        if (data.vertices.empty())
        {
            return nullptr;
        }
        // In place where the backend can; otherwise a new mesh
        if (!_quadMesh || !renderer->UpdateMesh(_quadMesh, data))
        {
            if (_quadMesh)
            {
                renderer->DestroyMesh(_quadMesh);
            }
            _quadMesh = renderer->CreateMesh(data);
        }
        _quadMeshQuads = _quadMesh ? std::move(local) : std::vector<Quad>{};
        return _quadMesh;
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
            if (_quadMesh)
            {
                _renderer->DestroyMesh(_quadMesh);
            }
        }
        // The sprite's share of its texture: destroyed with the last share, and only on a renderer that exists
        _spriteTexture.Release(true);
        _shaderSlot.Release(true); // the user shader's GPU material, likewise
        _mesh = nullptr;
        _material = nullptr;
        _quadMesh = nullptr;
        _quadMeshQuads.clear();
        _renderer = nullptr;
        _rendererLifetime.reset();
    }

    void Image::RenderUI(Renderer::Common::IRenderer *renderer, const Rect &rect,
                         const Renderer::Common::RenderState &state, const Matrix4 &canvasToWorld)
    {
#ifndef N2ENGINE_UI_ENABLED
        // A build without the UI module (N2ENGINE_USE_UI, defined on this file by engine/CMakeLists.txt) draws no UI
        static_cast<void>(renderer);
        static_cast<void>(rect);
        static_cast<void>(state);
        static_cast<void>(canvasToWorld);
#else
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

        // The geometry: the unit quad over the rect, or (issue #445) the quads a RectMask's clip leaves
        Renderer::Common::IMesh *mesh = _mesh;
        Matrix4 local = ModelMatrixFor(rect);
        // The texture's size, for 9-slicing: the raw texture wins over the sprite, as below
        Math::Vector2 textureSize{0.0f, 0.0f};
        if (_texture)
        {
            textureSize = Math::Vector2{static_cast<float>(_texture->GetWidth()), static_cast<float>(_texture->GetHeight())};
        }
        else if (_sprite)
        {
            textureSize = Math::Vector2{static_cast<float>(_sprite->GetWidth()), static_cast<float>(_sprite->GetHeight())};
        }
        if (const std::vector<Quad> quads = BuildQuads(rect, _renderClip, textureSize); !quads.empty())
        {
            if (quads.size() == 1 && !quads.front().position.HasArea())
            {
                return; // clipped away
            }
            const Math::Vector2 origin{rect.x, rect.y};
            mesh = EnsureQuadMesh(renderer, quads, origin);
            if (!mesh)
            {
                return;
            }
            local = Matrix4::identity();
            local(0, 3) = origin.x;
            local(1, 3) = origin.y;
        }

        // A user shader draws the sprite through the slot's own Material (a raw texture is not an asset a Material can hold, so it
        // keeps the standard path). The standard path below is not touched when there is no shader.
        if (_shaderSlot.HasShader() && !_texture)
        {
            _spriteTexture.Release(true); // the slot's Material holds the sprite's GPU texture itself
            const Common::Color shaderColor = GetDrawColor();
            _shaderSlot.Sync(_sprite, Renderer::Common::BlendCutOutBelow(_sprite != nullptr, renderer->SupportsBlending(), shaderColor.a));
            if (Renderer::Common::IMaterial *gpu = _shaderSlot.Prepare(*renderer, shaderColor))
            {
                Renderer::Common::RenderState shaderState = state;
                Rendering::MeshDrawing::ApplyBlendMode(shaderState, *_shaderSlot.GetMaterial());
                renderer->DrawMesh(mesh, ComposeModel(canvasToWorld, local).Data(), gpu, shaderState);
            }
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
        // A renderer that cannot blend (the software renderer) draws every pixel it keeps opaque. As Sprite2D does there, cut
        // out a texture's pixels under half the colour's alpha, so its transparent parts are not drawn as boxes, and a fully
        // faded image draws nothing (Renderer::Common::BlendCutOutBelow). A flat colour is never cut out: it is drawn opaque,
        // as before. 0 (no test) wherever the renderer blends.
        _material->SetFloat("uAlphaCutoff", Renderer::Common::BlendCutOutBelow(texture != nullptr,
                                                                              renderer->SupportsBlending(), color.a));
        // The colour effects (Renderer::Common::ColorEffects), clamped again: a scene file can hold any value
        const Common::Color flash = Renderer::Common::ColorEffects::ClampFlash(_flash);
        _material->SetFloat("uSaturation", Renderer::Common::ColorEffects::ClampSaturation(_saturation));
        _material->SetVec4("uFlash", flash.r, flash.g, flash.b, flash.a);

        const Matrix4 model = ComposeModel(canvasToWorld, local);
        renderer->DrawMesh(mesh, model.Data(), _material, state);
#endif
    }

    void Image::OnDestroy()
    {
        ReleaseResources();
    }
}
