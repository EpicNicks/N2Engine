#pragma once

#include <cstdint>

#include <renderer/common/Renderer.hpp>
#include <renderer/common/RenderState.hpp>
#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine
{
    /**
     * The pass a renderable draws in. Scene::Render draws every Opaque renderable first, in hierarchy
     * order, then every Transparent one, sorted. There is no overlay queue: overlay UI draws in its own
     * canvas pass (#42), and a world-space canvas is one Transparent renderable (UI::Canvas).
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
        /**
         * Draws this component. Scene::Render collects every renderable before drawing any, so Render and
         * RenderInQueue must not add, remove or destroy components or GameObjects immediately (that could
         * free a renderable still waiting to draw). Deferred destruction (Destroy) is fine: it takes
         * effect after rendering.
         */
        virtual void Render(Renderer::Common::IRenderer *renderer) = 0;
        virtual void InitializeRenderResources(Renderer::Common::IRenderer *renderer) = 0;
        virtual void CleanupRenderResources(Renderer::Common::IRenderer *renderer) = 0;

        /// The queue this renderable draws in, read once per frame by Scene::Render. Defaults to Opaque. Its
        /// sort key orders the renderable in the Transparent queue.
        [[nodiscard]] virtual RenderQueueKey GetRenderQueue() const { return {}; }

        /**
         * Whether this renderable draws in `queue` this frame, asked by Scene::Render for each queue (once per
         * frame, after GetRenderQueue). The default is the queue GetRenderQueue() names, and no other.
         *
         * A renderable that draws in both queues (a MeshRenderer whose submeshes have opaque and blended
         * materials) returns true for both: Scene::Render then calls RenderInQueue once for each, with the
         * queue, and the renderable draws only that queue's part each time. In the Transparent queue it sorts
         * by GetRenderQueue()'s sort key, whatever queue that names. The Opaque queue's order (hierarchy order)
         * is the same as for any other renderable.
         */
        [[nodiscard]] virtual bool DrawsInQueue(const RenderQueue queue) const { return queue == GetRenderQueue().queue; }

        /**
         * What Scene::Render calls, once per queue the renderable draws in (DrawsInQueue), with that queue and
         * its RenderState (RenderState::Opaque() or RenderState::Transparent()). The renderable passes `state`
         * to its DrawMesh calls, adjusting it if it needs to (e.g. cull None for a two-sided surface), and draws
         * only what belongs in `queue`.
         *
         * The default calls Render(renderer), whose draws get the default state, which is the Opaque
         * queue's. A Transparent renderable must override this to pass `state` on; otherwise its draws
         * are sorted as transparent but still write depth.
         *
         * The same rule as Render applies: no immediate changes to components or GameObjects.
         */
        virtual void RenderInQueue(Renderer::Common::IRenderer *renderer, const Renderer::Common::RenderState &state,
                                   const RenderQueue queue)
        {
            static_cast<void>(state);
            static_cast<void>(queue);
            Render(renderer);
        }
    };
}
