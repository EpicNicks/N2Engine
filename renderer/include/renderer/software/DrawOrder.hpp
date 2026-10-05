#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "renderer/common/RenderState.hpp"

namespace Renderer::Software
{
    /// What the software renderer's draw ordering needs to know about one recorded draw.
    struct DrawOrderKey
    {
        Common::RenderState state;
        /// Squared distance from the camera to the draw's model origin
        float distanceSq = 0.0f;
    };

    /**
     * Whether a draw may be moved for early-Z: it depth-tests and writes depth. Without blending (the
     * software renderer has none), where such a draw sits among other such draws cannot change the
     * image, apart from fragments at exactly equal depth.
     */
    [[nodiscard]] constexpr bool IsReorderable(const Common::RenderState &state)
    {
        return state.depthTest && state.depthWrite;
    }

    /**
     * The order the software renderer rasterizes a frame's draws in, as indices into `draws`.
     *
     * Each maximal run of consecutive reorderable draws is stable-sorted front to back (ascending
     * distanceSq; a NaN distance sorts last), so early-Z rejects occluded pixels before they are shaded.
     * Every other draw keeps its submission position, and no draw crosses it. A Transparent-queue draw
     * (depth write off), which Scene::Render submits after all opaque draws, is therefore rasterized after
     * them, in submission order.
     *
     * Text (the SDF text shader) alpha-tests and writes depth on this renderer even when its state says no
     * depth write, but it is ordered by the state it was submitted with. In the Transparent queue that
     * state doesn't write depth, so text keeps its submission position: after every opaque draw, back to
     * front among the transparent ones, as Scene::Render submits them. Its depth writes then only affect
     * draws submitted after it. Either way it would be correct: an alpha-tested, depth-writing draw is
     * order-independent among other depth-tested, depth-writing draws (its rejected pixels write nothing,
     * like occluded ones), so text drawn with an opaque state may be reordered like any opaque draw.
     */
    [[nodiscard]] std::vector<std::size_t> OrderDraws(std::span<const DrawOrderKey> draws);
}
