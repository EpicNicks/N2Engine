#include "engine/rendering/TextDrawing.hpp"

#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <initializer_list>
#include <set>
#include <string>
#include <utility>

#include <renderer/common/IShader.hpp>

#include "engine/Logger.hpp"

namespace N2Engine::Rendering::TextDrawing
{
    namespace
    {
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

        // Warn once per process for each component type (TextRenderer, UIText), not once per text or per frame
        bool FirstWarningFor(const std::string_view componentName)
        {
            static std::set<std::string, std::less<>> warned;
            return warned.emplace(componentName).second;
        }
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
        // The atlas share: the last user's release destroys the texture, on the same terms
        _atlas.Release(callRenderer);

        _mesh = nullptr;
        _material = nullptr;
        _meshVersion = 0;
        _renderer = nullptr;
        _rendererLifetime.reset();
    }

    bool DrawResources::EnsureAtlasTexture(const std::shared_ptr<Text::Font> &font)
    {
        if (_atlas && _atlas.GetSource() == font.get())
        {
            return true;
        }

        // Another font: give up the old atlas share (destroyed if this was its last user)
        _atlas.Release(true);

        _atlas = GpuCache::AcquireFontAtlas(*_renderer, font);
        if (!_atlas)
        {
            return false;
        }

        if (_material)
        {
            _material->SetTexture(_atlas.GetTexture());
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
            if (FirstWarningFor(componentName))
            {
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
            _material = renderer->CreateMaterial(shader, _atlas.GetTexture());
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
