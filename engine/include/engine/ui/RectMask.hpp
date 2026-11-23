#pragma once

#include <string>

#include "engine/serialization/ComponentSerializer.hpp"

namespace N2Engine::UI
{
    /**
     * Clips everything below its object to its object's rect (Unity's RectMask2D), issue #445. Put it on a UI element:
     * its children and their whole subtrees draw only inside its rect, and the pointer finds them only there, so a
     * button scrolled out of a ScrollView's viewport can't be clicked. The object's own graphics are not clipped by it
     * (they draw inside its rect anyway), only by masks above it.
     *
     * Masks nest: a mask below another clips to the overlap of both rects (Rect::Intersect). The clip is worked out by the
     * layout (UISystem) with the rects, so it follows the mask's rect as it moves or resizes, and it applies the same way on
     * a world-space canvas (in canvas units).
     *
     * How it clips: no GPU scissor. Each graphic cuts its own geometry to the clip rect on the CPU (an Image its quad and
     * its texture coordinates, a UIText each glyph quad), so every backend (OpenGL, Vulkan, WebGPU, software) draws the
     * same pixels, and a world-space canvas, which is not a screen rectangle, clips too. A disabled RectMask (or one on an
     * inactive object) clips nothing.
     */
    class RectMask final : public SerializableComponent
    {
    public:
        explicit RectMask(GameObject &gameObject) : SerializableComponent(gameObject) {}

        [[nodiscard]] std::string GetTypeName() const override { return "RectMask"; }

        /// One per object: AddComponent returns the existing one
        static constexpr bool IsSingleton = true;
    };
}
