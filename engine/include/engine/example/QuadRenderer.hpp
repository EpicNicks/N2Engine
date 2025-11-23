#pragma once

#include <math/Vector3.hpp>
#include <renderer/common/Renderer.hpp>
#include <renderer/common/IMaterial.hpp>
#include <renderer/common/IShader.hpp>

#include "engine/Component.hpp"
#include "engine/IRenderable.hpp"
#include "engine/common/Color.hpp"
#include "engine/common/ScriptUtils.hpp"

namespace N2Engine::Example
{
    class QuadRenderer : public IRenderable
    {
    private:
        Renderer::Common::IMesh *_mesh = 0;
        Renderer::Common::IMaterial *_material = nullptr;
        Renderer::Common::IShader *_shader = 0;
        bool _resourcesInitialized = false;

        Renderer::Common::IRenderer *_cachedRenderer = nullptr;

        // Rendering properties
        Common::Color _color{Common::Color::White()}; // White by default
        Math::Vector3 _size{Math::Vector3::One()};    // 1x1 quad by default

        void CreateQuadMesh(Renderer::Common::IRenderer *renderer);

    public:
        explicit QuadRenderer(GameObject &gameObject);

        std::string GetTypeName() const override { return NAMEOF(QuadRenderer); }

        // IRenderable interface
        void Render(Renderer::Common::IRenderer *renderer) override;
        void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
        void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;
        void OnDestroy() override;

        // Properties
        void SetColor(const Common::Color &color) { _color = color; }
        const Common::Color &GetColor() const { return _color; }

        void SetSize(const Math::Vector3 &size) { _size = size; }
        const Math::Vector3 &GetSize() const { return _size; }

        static constexpr bool IsSingleton = false;
    };
}
