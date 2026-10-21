#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Renderer::WebGpu
{
    /**
     * Copies `height` rows of `rowBytes` bytes out of a mapped readback buffer, whose rows are `paddedRowBytes` apart (a
     * texture-to-buffer copy pads each row to 256 bytes), into `out`, tightly packed. `out` is resized to
     * rowBytes * height. Returns false and leaves `out` as it was when `mapped` is null, `rowBytes` is 0, or
     * `paddedRowBytes` is smaller than `rowBytes`. The pure part of the browser's asynchronous ReadFramebuffer
     * (WebGpuRenderer.cpp, AdvanceAsyncReadback), which needs no GPU, so a test can run it.
     */
    inline bool StripRowPadding(const std::uint8_t* mapped, const std::size_t paddedRowBytes, const std::size_t rowBytes,
                                const std::size_t height, std::vector<std::uint8_t>& out)
    {
        if (mapped == nullptr || rowBytes == 0 || paddedRowBytes < rowBytes)
        {
            return false;
        }
        out.resize(rowBytes * height);
        for (std::size_t y = 0; y < height; ++y)
        {
            std::memcpy(out.data() + y * rowBytes, mapped + y * paddedRowBytes, rowBytes);
        }
        return true;
    }
}
