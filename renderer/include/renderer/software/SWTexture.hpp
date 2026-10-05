#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "renderer/common/ITexture.hpp"
#include "renderer/common/TextureOptions.hpp"

namespace Renderer::Software
{
    class SWTexture : public Common::ITexture
    {
    public:
        std::vector<uint8_t> data;
        uint32_t width = 0, height = 0, channels = 0;
        /// What the texture was created with. Both samplers honour wrap. Sample is always nearest (on the
        /// full-size image) whatever the filter; the text shader always uses SampleFirstChannelBilinear.
        /// mipmaps is stored but has no effect here.
        Common::TextureOptions options{};

        [[nodiscard]] bool IsValid() const override { return !data.empty(); }
        [[nodiscard]] uint32_t GetWidth() const override { return width; }
        [[nodiscard]] uint32_t GetHeight() const override { return height; }
        [[nodiscard]] uint32_t GetChannels() const override { return channels; }

        /**
         * Nearest sample at a normalized uv, packed RGBA8 (r in the low byte). Channels the texture lacks
         * read as OpenGL reads them from R8/RG8/RGB8 textures: green and blue 0, alpha 255. So a
         * single-channel texture samples as (r, 0, 0, 255) and a two-channel one as (r, g, 0, 255).
         */
        [[nodiscard]] uint32_t Sample(float u, float v) const
        {
            if (data.empty()) return 0xFFFFFFFF;
            if (options.wrap == Common::TextureWrap::ClampToEdge)
            {
                u = std::clamp(u, 0.0f, 1.0f);
                v = std::clamp(v, 0.0f, 1.0f);
            }
            else
            {
                u = u - std::floor(u); // wrap
                v = v - std::floor(v);
            }
            int x = (int)(u * (float)(width - 1));
            int y = (int)(v * (float)(height - 1));
            const uint8_t *p = data.data() + ((size_t)y * width + x) * channels;
            uint8_t r = channels > 0 ? p[0] : 0;
            uint8_t g = channels > 1 ? p[1] : 0;
            uint8_t b = channels > 2 ? p[2] : 0;
            uint8_t a = channels > 3 ? p[3] : 255;
            return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }

        /**
         * The first channel (red), bilinearly filtered, from 0 to 1, as OpenGL's GL_LINEAR filters without
         * mipmaps: texel centres sit at (i + 0.5) / size, and the four texels around the sample point are
         * weighted by distance. Neighbours outside the texture are clamped to the edge or wrapped around,
         * per options.wrap. 1 for a texture without data; a NaN coordinate reads as 0.
         */
        [[nodiscard]] float SampleFirstChannelBilinear(float u, float v) const
        {
            if (data.empty() || width == 0 || height == 0 || channels == 0) return 1.0f;
            if (std::isnan(u)) u = 0.0f;
            if (std::isnan(v)) v = 0.0f;

            const bool clampToEdge = options.wrap == Common::TextureWrap::ClampToEdge;
            // Bring the coordinate into 0..1 first, so the texel indices below stay small
            if (clampToEdge)
            {
                u = std::clamp(u, 0.0f, 1.0f);
                v = std::clamp(v, 0.0f, 1.0f);
            }
            else
            {
                u = u - std::floor(u);
                v = v - std::floor(v);
            }

            const float fx = u * (float)width - 0.5f;
            const float fy = v * (float)height - 0.5f;
            const float x0f = std::floor(fx);
            const float y0f = std::floor(fy);
            const float tx = fx - x0f;
            const float ty = fy - y0f;

            const int w = (int)width;
            const int h = (int)height;
            auto index = [clampToEdge](int i, int size)
            {
                return clampToEdge ? std::clamp(i, 0, size - 1) : ((i % size) + size) % size;
            };
            const int x0 = index((int)x0f, w);
            const int x1 = index((int)x0f + 1, w);
            const int y0 = index((int)y0f, h);
            const int y1 = index((int)y0f + 1, h);

            auto texel = [&](int x, int y)
            {
                return (float)data[((size_t)y * width + (size_t)x) * channels];
            };
            const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * tx;
            const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * tx;
            return (top + (bottom - top) * ty) * (1.0f / 255.0f);
        }
    };
}
