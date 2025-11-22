#pragma once

#include <renderer/common/Renderer.hpp>
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine
{
    /**
     * An interface for components which should render to the screen
     */
    class IRenderable : public SerializableComponent
    {
        using SerializableComponent::SerializableComponent;

    public:
        virtual void Render(Renderer::Common::IRenderer *renderer) = 0;
        virtual void InitializeRenderResources(Renderer::Common::IRenderer *renderer) = 0;
        virtual void CleanupRenderResources(Renderer::Common::IRenderer *renderer) = 0;
    };
}