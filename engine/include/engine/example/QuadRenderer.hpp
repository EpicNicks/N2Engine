#pragma once

#include "engine/Component.hpp"
#include "engine/IRenderable.hpp"
#include "renderer/common/Renderer.hpp"
#include "engine/common/Color.hpp"
#include <math/Vector3.hpp>

namespace N2Engine
{
    namespace Example
    {
        class QuadRenderer : public IRenderable
        {
        private:
            uint32_t _meshId = 0;
            uint32_t _materialId = 0;
            uint32_t _shaderId = 0;
            bool _resourcesInitialized = false;

            // Rendering properties
            Common::Color _color{Common::Color::White()}; // White by default
            Math::Vector3 _size{Math::Vector3::One()};    // 1x1 quad by default

            void CreateQuadMesh(Renderer::Common::IRenderer *renderer);

        public:
            QuadRenderer(GameObject &gameObject);
            virtual ~QuadRenderer();

            // IRenderable interface
            void Render(Renderer::Common::IRenderer *renderer) override;
            void InitializeRenderResources(Renderer::Common::IRenderer *renderer) override;
            void CleanupRenderResources(Renderer::Common::IRenderer *renderer) override;

            void OnUpdate() override;

            // Properties
            void SetColor(const Common::Color &color) { _color = color; }
            const Common::Color &GetColor() const { return _color; }

            void SetSize(const Math::Vector3 &size) { _size = size; }
            const Math::Vector3 &GetSize() const { return _size; }

            static constexpr bool IsSingleton = false;
        };
    }
}