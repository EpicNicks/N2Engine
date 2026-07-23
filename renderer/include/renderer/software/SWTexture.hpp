#pragma once

#include <cmath>
#include <vector>

#include "renderer/common/ITexture.hpp"

namespace Renderer::Software
{
    class SWTexture : public Common::ITexture
    {
    public:
        std::vector<uint8_t> data;
        uint32_t width = 0, height = 0, channels = 0;

        [[nodiscard]] bool IsValid() const override { return !data.empty(); }
        [[nodiscard]] uint32_t GetWidth() const override { return width; }
        [[nodiscard]] uint32_t GetHeight() const override { return height; }
        [[nodiscard]] uint32_t GetChannels() const override { return channels; }

        // Sample at normalized UV (nearest)
        [[nodiscard]] uint32_t Sample(float u, float v) const
        {
            if (data.empty()) return 0xFFFFFFFF;
            u = u - std::floor(u); // wrap
            v = v - std::floor(v);
            int x = (int)(u * (float)(width - 1));
            int y = (int)(v * (float)(height - 1));
            const uint8_t *p = data.data() + (y * width + x) * channels;
            uint8_t r = channels > 0 ? p[0] : 255;
            uint8_t g = channels > 1 ? p[1] : 255;
            uint8_t b = channels > 2 ? p[2] : 255;
            uint8_t a = channels > 3 ? p[3] : 255;
            return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }
    };
}
