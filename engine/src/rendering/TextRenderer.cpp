#include "engine/rendering/TextRenderer.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <map>
#include <utility>

#include <renderer/common/TextureOptions.hpp>

#include "engine/GameObjectScene.hpp"
#include "engine/Logger.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"
#include "engine/text/TextJson.hpp"

namespace N2Engine::Rendering
{
    namespace
    {
        using Renderer::Common::IRenderer;
        using Renderer::Common::ITexture;

        // One atlas texture per (renderer, font), shared by every TextRenderer drawing that font there.
        // The entry holds the font, so the font's address (half the key) stays valid while it exists. The
        // renderer's address can be reused by a later renderer, so the entry also keeps the renderer's
        // lifetime token: an entry whose renderer is gone is never handed out or destroyed.
        struct AtlasTexture
        {
            std::shared_ptr<Text::Font> font;
            std::weak_ptr<const void> rendererLifetime;
            ITexture *texture = nullptr;
            std::size_t users = 0;
        };

        using AtlasKey = std::pair<IRenderer *, const Text::Font *>;

        bool SameOwner(const std::weak_ptr<const void> &a, const std::weak_ptr<const void> &b)
        {
            return !a.owner_before(b) && !b.owner_before(a);
        }

        std::map<AtlasKey, AtlasTexture> &AtlasTextures()
        {
            // Leaked on purpose: a TextRenderer kept alive by Lua can be destroyed after function-local statics
            static auto *textures = new std::map<AtlasKey, AtlasTexture>();
            return *textures;
        }

        ITexture *AcquireAtlasTexture(IRenderer &renderer, const std::shared_ptr<Text::Font> &font)
        {
            auto &textures = AtlasTextures();
            const AtlasKey key{&renderer, font.get()};
            const std::weak_ptr<const void> lifetime = renderer.GetLifetimeToken();
            if (const auto it = textures.find(key); it != textures.end())
            {
                if (SameOwner(it->second.rendererLifetime, lifetime))
                {
                    ++it->second.users;
                    return it->second.texture;
                }
                // Left by a destroyed renderer at the same address: its texture went with it
                textures.erase(it);
            }

            const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
            if (atlas.GetWidth() <= 0 || atlas.GetHeight() <= 0 || atlas.GetPixels().empty())
            {
                return nullptr;
            }

            ITexture *texture = renderer.CreateTexture(atlas.GetPixels().data(), static_cast<uint32_t>(atlas.GetWidth()),
                                                       static_cast<uint32_t>(atlas.GetHeight()), 1,
                                                       Renderer::Common::TextureOptions::SdfAtlas());
            if (!texture)
            {
                return nullptr;
            }
            textures.emplace(key, AtlasTexture{font, lifetime, texture, 1});
            return texture;
        }

        /// rendererLifetime is the token the caller held when it took the texture: an entry made since by
        /// another renderer at the same address isn't the caller's to release.
        void ReleaseAtlasTexture(IRenderer *renderer, const std::weak_ptr<const void> &rendererLifetime,
                                 const Text::Font *font, const bool callRenderer)
        {
            auto &textures = AtlasTextures();
            const auto it = textures.find(AtlasKey{renderer, font});
            if (it == textures.end() || !SameOwner(it->second.rendererLifetime, rendererLifetime))
            {
                return;
            }
            if (--it->second.users > 0)
            {
                return;
            }
            if (callRenderer && renderer && !rendererLifetime.expired())
            {
                renderer->DestroyTexture(it->second.texture);
            }
            textures.erase(it);
        }

        bool SameFloat(const float a, const float b)
        {
            return a == b || (std::isnan(a) && std::isnan(b));
        }

        // Warn once per process, not once per text or per frame
        bool g_warnedNoTextShader = false;
    }

    TextRenderer::TextRenderer(GameObject &gameObject) : IRenderable(gameObject)
    {
        _gameObject.CreatePositionable();
        RegisterMember(NAMEOF(_text), _text);
        RegisterAssetRef(NAMEOF(_font), _font);
        RegisterMember(NAMEOF(_fontSize), _fontSize);
        RegisterMember(NAMEOF(_color), _color);
        RegisterMember(NAMEOF(_horizontalAlign), _horizontalAlign);
        RegisterMember(NAMEOF(_verticalAlign), _verticalAlign);
        RegisterMember(NAMEOF(_maxWidth), _maxWidth);
        RegisterMember(NAMEOF(_lineSpacing), _lineSpacing);
        RegisterMember(NAMEOF(_letterSpacing), _letterSpacing);
    }

    TextRenderer::~TextRenderer()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its
        // GameObject never joined a scene) may outlive its renderer, so the renderer isn't called.
        ReleaseRenderResources(false);
    }

    bool TextRenderer::LayoutInputs::MatchesSettings(const LayoutInputs &other) const
    {
        return font == other.font && SameFloat(fontSize, other.fontSize) &&
               SameFloat(maxWidth, other.maxWidth) && SameFloat(lineSpacing, other.lineSpacing) &&
               SameFloat(letterSpacing, other.letterSpacing) && horizontalAlign == other.horizontalAlign &&
               verticalAlign == other.verticalAlign;
    }

    std::shared_ptr<Text::Font> TextRenderer::GetEffectiveFont() const
    {
        return _font ? _font : Text::Font::GetDefault();
    }

    Text::LayoutOptions TextRenderer::GetLayoutOptions() const
    {
        Text::LayoutOptions options;
        options.fontSize = _fontSize;
        options.maxWidth = _maxWidth;
        options.horizontalAlign = _horizontalAlign;
        options.verticalAlign = _verticalAlign;
        options.lineSpacing = _lineSpacing;
        options.letterSpacing = _letterSpacing;
        return options;
    }

    const Text::TextLayout &TextRenderer::GetLayout() const
    {
        std::shared_ptr<Text::Font> font = GetEffectiveFont();
        // The text is compared in place and copied only when the layout changes, not every frame
        LayoutInputs inputs{
            .text = {},
            .font = font.get(),
            .fontSize = _fontSize,
            .maxWidth = _maxWidth,
            .lineSpacing = _lineSpacing,
            .letterSpacing = _letterSpacing,
            .horizontalAlign = _horizontalAlign,
            .verticalAlign = _verticalAlign,
        };

        if (_layoutInputs && _layoutInputs->text == _text && _layoutInputs->MatchesSettings(inputs))
        {
            return _layout;
        }

        _layout = font ? font->Layout(_text, GetLayoutOptions()) : Text::TextLayout{};
        inputs.text = _text;
        _layoutInputs = std::move(inputs);
        _layoutFont = std::move(font);
        ++_layoutVersion;
        return _layout;
    }

    Renderer::Common::MeshData TextRenderer::BuildMesh(const Text::TextLayout &layout)
    {
        Renderer::Common::MeshData data;
        data.vertices.reserve(layout.quads.size() * 4);
        data.indices.reserve(layout.quads.size() * 6);

        const auto corner = [](const float x, const float y, const float u, const float v)
        {
            return Renderer::Common::Vertex{{x, y, 0.0f}, {0.0f, 0.0f, 1.0f}, {u, v}, {1.0f, 1.0f, 1.0f, 1.0f}};
        };

        for (const Text::GlyphQuad &quad : layout.quads)
        {
            const auto base = static_cast<uint32_t>(data.vertices.size());
            const Text::Rect &p = quad.position; // y-up
            const Text::Rect &uv = quad.uv;      // y-down: minY is the glyph's top row

            data.vertices.push_back(corner(p.minX, p.minY, uv.minX, uv.maxY)); // bottom-left
            data.vertices.push_back(corner(p.maxX, p.minY, uv.maxX, uv.maxY)); // bottom-right
            data.vertices.push_back(corner(p.maxX, p.maxY, uv.maxX, uv.minY)); // top-right
            data.vertices.push_back(corner(p.minX, p.maxY, uv.minX, uv.minY)); // top-left

            for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u})
            {
                data.indices.push_back(base + index);
            }
        }
        return data;
    }

    void TextRenderer::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent());
    }

    bool TextRenderer::HoldsRenderer(const Renderer::Common::IRenderer *renderer) const
    {
        return renderer && renderer == _renderer && !_rendererLifetime.expired();
    }

    void TextRenderer::BindRenderer(Renderer::Common::IRenderer *renderer)
    {
        if (HoldsRenderer(renderer))
        {
            return;
        }
        // Everything held belongs to the previous renderer, which is only called if it still exists
        ReleaseRenderResources(true);
        _renderer = renderer;
        _rendererLifetime = renderer->GetLifetimeToken();
    }

    void TextRenderer::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            BindRenderer(renderer);
        }
    }

    void TextRenderer::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || renderer != _renderer)
        {
            return;
        }
        ReleaseRenderResources(true);
    }

    void TextRenderer::OnDestroy()
    {
        CleanupRenderResources(_renderer);
    }

    void TextRenderer::ReleaseRenderResources(bool callRenderer)
    {
        // A destroyed renderer freed everything itself, and its address may now be another renderer's
        callRenderer = callRenderer && _renderer && !_rendererLifetime.expired();
        if (callRenderer)
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
        if (_atlasFont)
        {
            ReleaseAtlasTexture(_renderer, _rendererLifetime, _atlasFont, callRenderer);
        }

        _mesh = nullptr;
        _material = nullptr;
        _atlasTexture = nullptr;
        _atlasFont = nullptr;
        _meshVersion = 0;
        _renderer = nullptr;
        _rendererLifetime.reset();
    }

    bool TextRenderer::EnsureAtlasTexture(const std::shared_ptr<Text::Font> &font)
    {
        if (_atlasTexture && _atlasFont == font.get())
        {
            return true;
        }

        if (_atlasFont)
        {
            ReleaseAtlasTexture(_renderer, _rendererLifetime, _atlasFont, true);
            _atlasTexture = nullptr;
            _atlasFont = nullptr;
        }

        ITexture *texture = AcquireAtlasTexture(*_renderer, font);
        if (!texture)
        {
            return false;
        }
        _atlasTexture = texture;
        _atlasFont = font.get();

        if (_material)
        {
            _material->SetTexture(_atlasTexture);
        }
        return true;
    }

    bool TextRenderer::EnsureMesh(const Text::TextLayout &layout)
    {
        if (_mesh && _meshVersion == _layoutVersion)
        {
            return true;
        }

        const Renderer::Common::MeshData data = BuildMesh(layout);
        if (_mesh && !_renderer->UpdateMesh(_mesh, data))
        {
            // The backend can't update meshes in place: replace it
            _renderer->DestroyMesh(_mesh);
            _mesh = nullptr;
        }
        if (!_mesh)
        {
            _mesh = _renderer->CreateMesh(data);
            if (!_mesh)
            {
                return false;
            }
        }
        _meshVersion = _layoutVersion;
        return true;
    }

    void TextRenderer::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state)
    {
        if (!renderer)
        {
            return;
        }
        BindRenderer(renderer);

        const Text::TextLayout &layout = GetLayout();
        const std::shared_ptr<Text::Font> font = _layoutFont;
        if (!font || !font->IsLoaded() || layout.quads.empty())
        {
            return;
        }

        Renderer::Common::IShader *shader = renderer->GetStandardTextShader();
        if (!shader)
        {
            if (!g_warnedNoTextShader)
            {
                g_warnedNoTextShader = true;
                Logger::Warn(std::format("TextRenderer: the {} has no text shader, so text isn't drawn",
                                         renderer->GetRendererName()));
            }
            return;
        }

        if (!EnsureAtlasTexture(font))
        {
            return;
        }
        if (!_material)
        {
            _material = renderer->CreateMaterial(shader, _atlasTexture);
            if (!_material)
            {
                return;
            }
        }
        if (!EnsureMesh(layout))
        {
            return;
        }

        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return;
        }
        const Positionable::Matrix4 world = positionable->GetLocalToWorldMatrix();

        _material->SetColor("uAlbedo", _color.r, _color.g, _color.b, _color.a);
        renderer->DrawMesh(_mesh, world.Data(), _material, state);
    }
}
