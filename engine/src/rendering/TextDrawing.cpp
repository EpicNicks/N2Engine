#include "engine/rendering/TextDrawing.hpp"

#include <algorithm>
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

    // ===== Effects =====

    float MaxEffectEms(const Text::AtlasSettings &settings)
    {
        if (!(settings.basePx > 0.0f) || settings.spreadPx <= 0)
        {
            return 0.0f;
        }
        return kUsableSpread * static_cast<float>(settings.spreadPx) / settings.basePx;
    }

    EffectUniforms ResolveEffects(const Text::TextEffects &effects, const Text::AtlasSettings &settings,
                                  const int atlasWidth, const int atlasHeight)
    {
        EffectUniforms out;
        const float budget = MaxEffectEms(settings);
        if (!(budget > 0.0f) || atlasWidth <= 0 || atlasHeight <= 0)
        {
            return out;
        }
        // Distance units per em: the atlas changes by 0.5 over spreadPx pixels, and an em is basePx pixels
        const float valuePerEm = 0.5f * settings.basePx / static_cast<float>(settings.spreadPx);

        // A length clamped to [0, limit]; a negative or non-finite one counts as 0
        const auto clampLength = [&out](const float value, const float limit)
        {
            const float length = std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
            if (length > limit)
            {
                out.clamped = true;
                return limit;
            }
            return length;
        };

        // The outline first, then each softness in what is left of the spread
        const float outline = clampLength(effects.outlineWidth, budget);
        if (outline > 0.0f)
        {
            out.outline = outline * valuePerEm;
            out.outlineColor = effects.outlineColor;
        }
        const float softHalf = clampLength(effects.softness * 0.5f, budget - outline);
        out.softness = softHalf * valuePerEm;

        if (effects.HasShadow())
        {
            out.shadowColor = effects.shadowColor;
            const float shadowSoftHalf = clampLength(effects.shadowSoftness * 0.5f, budget - outline);
            out.shadowSoftness = shadowSoftHalf * valuePerEm;

            // The offset in each axis gets what the outline and the shadow's softness leave
            const float offsetLimit = std::max(budget - outline - shadowSoftHalf, 0.0f);
            const auto clampOffset = [&](const float value)
            {
                if (!std::isfinite(value))
                {
                    return 0.0f;
                }
                const float magnitude = clampLength(std::abs(value), offsetLimit);
                return value < 0.0f ? -magnitude : magnitude;
            };
            const float offsetX = clampOffset(effects.shadowOffset.x);
            const float offsetY = clampOffset(effects.shadowOffset.y);
            // An em is basePx atlas pixels. The atlas uv is y-down, so a shadow moved up (+y) is one moved
            // to smaller v.
            out.shadowOffsetU = offsetX * settings.basePx / static_cast<float>(atlasWidth);
            out.shadowOffsetV = -offsetY * settings.basePx / static_cast<float>(atlasHeight);
        }
        return out;
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
                             const Common::Color &color, const Text::TextEffects &effects,
                             const Renderer::Common::RenderState &state, const std::string_view componentName)
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

        // The effects, set on every draw: materials share the text program, which keeps the last values set
        const Text::FontAtlas &atlas = font->GetSdfFont().GetAtlas();
        const Text::AtlasSettings &atlasSettings = atlas.GetSettings();
        const EffectUniforms fx = ResolveEffects(effects, atlasSettings, atlas.GetWidth(), atlas.GetHeight());
        if (fx.clamped && FirstWarningFor(std::string(componentName) + " effects"))
        {
            Logger::Warn(std::format(
                "{}: text effects were reduced to fit the font's SDF spread. Outline + softness / 2, and each shadow "
                "offset + outline + shadowSoftness / 2, can be at most {} em with this font (spreadPx {} at basePx {}); "
                "a larger spreadPx in the font's .meta allows more.",
                componentName, MaxEffectEms(atlasSettings), atlasSettings.spreadPx, atlasSettings.basePx));
        }
        _material->SetFloat("uOutline", fx.outline);
        _material->SetColor("uOutlineColor", fx.outlineColor.r, fx.outlineColor.g, fx.outlineColor.b,
                            fx.outlineColor.a);
        _material->SetFloat("uSoftness", fx.softness);
        _material->SetColor("uShadowColor", fx.shadowColor.r, fx.shadowColor.g, fx.shadowColor.b, fx.shadowColor.a);
        _material->SetVec2("uShadowOffset", fx.shadowOffsetU, fx.shadowOffsetV);
        _material->SetFloat("uShadowSoftness", fx.shadowSoftness);
        renderer->DrawMesh(_mesh, modelMatrix, _material, state);
        return true;
    }
}
