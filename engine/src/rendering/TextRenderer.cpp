#include "engine/rendering/TextRenderer.hpp"

#include <utility>

#include "engine/GameObjectScene.hpp"
#include "engine/Positionable.hpp"
#include "engine/common/ScriptUtils.hpp"
#include "engine/serialization/MathSerialization.hpp"
#include "engine/text/TextJson.hpp"

namespace N2Engine::Rendering
{
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
        // The effects, one key each (a scene saved before effects existed loads with them all off)
        RegisterMember("_outlineWidth", _effects.outlineWidth);
        RegisterMember("_outlineColor", _effects.outlineColor);
        RegisterMember("_shadowOffset", _effects.shadowOffset);
        RegisterMember("_shadowColor", _effects.shadowColor);
        RegisterMember("_shadowSoftness", _effects.shadowSoftness);
        RegisterMember("_softness", _effects.softness);
    }

    TextRenderer::~TextRenderer()
    {
        // Normally OnDestroy has released everything already. A component freed without it (its
        // GameObject never joined a scene) may outlive its renderer, so the renderer isn't called.
        _resources.Release(false);
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
        return _layoutCache.Get(_text, GetEffectiveFont(), GetLayoutOptions());
    }

    Renderer::Common::MeshData TextRenderer::BuildMesh(const Text::TextLayout &layout)
    {
        return TextDrawing::BuildMesh(layout);
    }

    void TextRenderer::Render(Renderer::Common::IRenderer *renderer)
    {
        RenderInQueue(renderer, Renderer::Common::RenderState::Transparent(), RenderQueue::Transparent);
    }

    void TextRenderer::InitializeRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (renderer)
        {
            _resources.Bind(renderer);
        }
    }

    void TextRenderer::CleanupRenderResources(Renderer::Common::IRenderer *renderer)
    {
        if (!renderer || renderer != _resources.GetRenderer())
        {
            return;
        }
        _resources.Release(true);
    }

    void TextRenderer::OnDestroy()
    {
        CleanupRenderResources(_resources.GetRenderer());
    }

    void TextRenderer::RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                     const RenderQueue queue)
    {
        static_cast<void>(queue); // it only draws in the Transparent queue
        if (!renderer)
        {
            return;
        }
        _resources.Bind(renderer);

        const Text::TextLayout &layout = GetLayout();
        const Positionable *positionable = GetGameObject().GetPositionable();
        if (!positionable)
        {
            return;
        }
        const Positionable::Matrix4 world = positionable->GetLocalToWorldMatrix();
        _resources.Draw(_layoutCache.GetFont(), layout, _layoutCache.GetVersion(), world.Data(), _color,
                        _effects, state, "TextRenderer");
    }
}
