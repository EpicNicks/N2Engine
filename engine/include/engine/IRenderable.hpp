#pragma once

#include <renderer/common/Renderer.hpp>
#include "engine/Component.hpp"

namespace N2Engine
{
    class IRenderable : public Component
    {
        using Component::Component;

    public:
        virtual ~IRenderable() = default;
        virtual void Render(Renderer::Common::IRenderer *renderer) = 0;
        virtual void InitializeRenderResources(Renderer::Common::IRenderer *renderer) = 0;
        virtual void CleanupRenderResources(Renderer::Common::IRenderer *renderer) = 0;
    };
}