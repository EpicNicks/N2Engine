#include "engine/rendering/TextDrawing.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <map>
#include <utility>

#include <renderer/common/IShader.hpp>
#include <renderer/common/TextureOptions.hpp>

#include "engine/Logger.hpp"

namespace N2Engine::Rendering::TextDrawing
{
    namespace
    {
        using Renderer::Common::IRenderer;
        using Renderer::Common::ITexture;

        // One atlas texture per (renderer, font), shared by every text component drawing that font there.
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
            // Leaked on purpose: a text component kept alive by Lua can be destroyed after function-local
            // statics
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

        bool SameOptions(const Text::LayoutOptions &a, const Text::LayoutOptions &b)
        {
            return SameFloat(a.fontSize, b.fontSize) && SameFloat(a.maxWidth, b.maxWidth) &&
                   a.horizontalAlign == b.horizontalAlign && a.verticalAlign == b.verticalAlign &&
                   SameFloat(a.lineSpacing, b.lineSpacing) && SameFloat(a.letterSpacing, b.letterSpacing) &&
                   SameFloat(a.tabWidth, b.tabWidth) && a.shaper == b.shaper;
        }

        // Warn once per process, not once per text or per frame
        bool g_warnedNoTextShader = false;
    }

    Renderer::Common::MeshData BuildMesh(const Text::TextLayout &layout)
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

    // ===== LayoutCache =====

    const Text::TextLayout &LayoutCache::Get(const std::string &text, std::shared_ptr<Text::Font> font,
                                             const Text::LayoutOptions &options)
    {
        if (_inputs && _inputs->font == font.get() && SameOptions(_inputs->options, options) &&
            _inputs->text == text)
        {
            return _layout;
        }

        _layout = font ? font->Layout(text, options) : Text::TextLayout{};
        _inputs = Inputs{.text = text, .font = font.get(), .options = options};
        _font = std::move(font);
        ++_version;
        return _layout;
    }

    // ===== DrawResources =====

    DrawResources::~DrawResources()
    {
        Release(false);
    }

    bool DrawResources::Holds(const Renderer::Common::IRenderer *renderer) const
    {
        return renderer && renderer == _renderer && !_rendererLifetime.expired();
    }

    void DrawResources::Bind(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || Holds(renderer))
        {
            return;
        }
        // Everything held belongs to the previous renderer, which is only called if it still exists
        Release(true);
        _renderer = renderer;
        _rendererLifetime = renderer->GetLifetimeToken();
    }

    void DrawResources::Release(bool callRenderer)
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

    bool DrawResources::EnsureAtlasTexture(const std::shared_ptr<Text::Font> &font)
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

    bool DrawResources::EnsureMesh(const Text::TextLayout &layout, const std::uint64_t layoutVersion)
    {
        if (_mesh && _meshVersion == layoutVersion)
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
        _meshVersion = layoutVersion;
        return true;
    }

    bool DrawResources::Draw(const std::shared_ptr<Text::Font> &font, const Text::TextLayout &layout,
                             const std::uint64_t layoutVersion, const float *modelMatrix,
                             const Common::Color &color, const Renderer::Common::RenderState &state,
                             const std::string_view componentName)
    {
        Renderer::Common::IRenderer *renderer = _renderer;
        if (!renderer || _rendererLifetime.expired() || !modelMatrix)
        {
            return false;
        }
        if (!font || !font->IsLoaded() || layout.quads.empty())
        {
            return false;
        }

        Renderer::Common::IShader *shader = renderer->GetStandardTextShader();
        if (!shader)
        {
            if (!g_warnedNoTextShader)
            {
                g_warnedNoTextShader = true;
                Logger::Warn(std::format("{}: the {} has no text shader, so text isn't drawn", componentName,
                                         renderer->GetRendererName()));
            }
            return false;
        }

        if (!EnsureAtlasTexture(font))
        {
            return false;
        }
        if (!_material)
        {
            _material = renderer->CreateMaterial(shader, _atlasTexture);
            if (!_material)
            {
                return false;
            }
        }
        if (!EnsureMesh(layout, layoutVersion))
        {
            return false;
        }

        _material->SetColor("uAlbedo", color.r, color.g, color.b, color.a);
        renderer->DrawMesh(_mesh, modelMatrix, _material, state);
        return true;
    }
}
