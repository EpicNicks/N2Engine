#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Renderer::Common
{
    /**
     * Reverses the order of the rows of an image in place: the first row becomes the last. Turns
     * ReadFramebuffer's bottom-row-first frames into top-row-first ones (what images and canvases expect) and
     * back. `pixels` holds `height` rows of `rowBytes` bytes each; nothing happens when it is null or either
     * count is 0. Each row keeps its bytes as they are, so any pixel format works.
     */
    inline void FlipRows(std::uint8_t *pixels, const std::size_t rowBytes, const std::size_t height)
    {
        if (!pixels || rowBytes == 0)
        {
            return;
        }
        for (std::size_t top = 0, bottom = height; top + 1 < bottom; ++top, --bottom)
        {
            std::swap_ranges(pixels + top * rowBytes, pixels + (top + 1) * rowBytes,
                             pixels + (bottom - 1) * rowBytes);
        }
    }
}
