#pragma once

#include <renderer/common/Renderer.hpp>

namespace N2Engine
{
    class IRenderable
    {
    public:
        virtual ~IRenderable() = default;
        virtual void Render(Renderer::Common::IRenderer *renderer) = 0;
        virtual void InitializeRenderResources(Renderer::Common::IRenderer *renderer) = 0;
        virtual void CleanupRenderResources(Renderer::Common::IRenderer *renderer) = 0;
    };
}