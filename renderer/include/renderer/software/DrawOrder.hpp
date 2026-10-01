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
     */
    [[nodiscard]] std::vector<std::size_t> OrderDraws(std::span<const DrawOrderKey> draws);
}
