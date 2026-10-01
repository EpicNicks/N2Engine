#pragma once

#include <cstdint>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine
{
    /**
     * The pass a renderable draws in. Scene::Render draws every Opaque renderable first, in hierarchy
     * order, then every Transparent one, sorted. There is no overlay queue: UI draws in its own canvas
     * pass (#42).
     */
    enum class RenderQueue : std::uint8_t
    {
        Opaque,
        Transparent
    };

    /// Where a renderable sits in the frame's draw order (IRenderable::GetRenderQueue)
    struct RenderQueueKey
    {
        RenderQueue queue = RenderQueue::Opaque;
        /// Orders Transparent renderables before depth: lower keys draw first (further back). Ignored in the
        /// Opaque queue, which keeps hierarchy order.
        int sortKey = 0;

        friend constexpr bool operator==(const RenderQueueKey &, const RenderQueueKey &) = default;
    };

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

        /// The queue this renderable draws in, read once per frame by Scene::Render. Defaults to Opaque.
        [[nodiscard]] virtual RenderQueueKey GetRenderQueue() const { return {}; }

        /**
         * What Scene::Render calls, with the RenderState of this renderable's queue
         * (RenderState::Opaque() or RenderState::Transparent()). The renderable passes `state` to its
         * DrawMesh calls, adjusting it if it needs to (e.g. cull None for a two-sided surface).
         *
         * The default calls Render(renderer), whose draws get the default state, which is the Opaque
         * queue's. A Transparent renderable must override this to pass `state` on; otherwise its draws
         * are sorted as transparent but still write depth.
         */
        virtual void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state)
        {
            static_cast<void>(state);
            Render(renderer);
        }
    };
}
