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
        /// What the texture was created with. Every sampler honours wrap. The unlit and lit shaders sample
        /// through SampleFiltered, which follows the filter (bilinear for Linear, nearest for Nearest); the text
        /// shader always uses SampleFirstChannelBilinear. mipmaps is stored but has no effect here: a texture
        /// is always sampled at full size.
        Common::TextureOptions options{};

        [[nodiscard]] bool IsValid() const override { return !data.empty(); }
        [[nodiscard]] uint32_t GetWidth() const override { return width; }
        [[nodiscard]] uint32_t GetHeight() const override { return height; }
        [[nodiscard]] uint32_t GetChannels() const override { return channels; }

        /**
         * Nearest sample at a normalized uv, packed RGBA8 (r in the low byte), as OpenGL's GL_NEAREST picks
         * texels: texel i covers u from i / size to (i + 1) / size. Channels the texture lacks read as OpenGL
         * reads them from R8/RG8/RGB8 textures: green and blue 0, alpha 255. So a single-channel texture
         * samples as (r, 0, 0, 255) and a two-channel one as (r, g, 0, 255). A NaN or infinite coordinate
         * reads as 0.
         */
        [[nodiscard]] uint32_t Sample(float u, float v) const
        {
            if (data.empty() || width == 0 || height == 0 || channels == 0) return 0xFFFFFFFF;
            if (!std::isfinite(u)) u = 0.0f; // NaN or inf would make a garbage texel index
            if (!std::isfinite(v)) v = 0.0f;
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
            // u = 1 (clamped) belongs to the last texel
            const int x = std::min((int)(u * (float)width), (int)width - 1);
            const int y = std::min((int)(v * (float)height), (int)height - 1);
            const uint8_t *p = data.data() + ((size_t)y * width + (size_t)x) * channels;
            uint8_t r = channels > 0 ? p[0] : 0;
            uint8_t g = channels > 1 ? p[1] : 0;
            uint8_t b = channels > 2 ? p[2] : 0;
            uint8_t a = channels > 3 ? p[3] : 255;
            return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
        }

        /**
         * Every channel bilinearly filtered, packed RGBA8 like Sample (missing channels read as Sample reads
         * them), with the same texel centres, weights and wrapping as SampleFirstChannelBilinear: OpenGL's
         * GL_LINEAR without mipmaps. Each channel is rounded to the nearest 8-bit value. 0xFFFFFFFF for a
         * texture without data; a NaN or infinite coordinate reads as 0.
         */
        [[nodiscard]] uint32_t SampleBilinear(float u, float v) const
        {
            if (data.empty() || width == 0 || height == 0 || channels == 0) return 0xFFFFFFFF;
            const BilinearTaps t = TapsAt(u, v);

            auto channel = [&](int x, int y, uint32_t c) -> float
            {
                if (c < channels) return (float)data[((size_t)y * width + (size_t)x) * channels + c];
                return c == 3 ? 255.0f : 0.0f;
            };
            uint32_t packed = 0;
            for (uint32_t c = 0; c < 4; ++c)
            {
                const float top = channel(t.x0, t.y0, c) + (channel(t.x1, t.y0, c) - channel(t.x0, t.y0, c)) * t.tx;
                const float bottom = channel(t.x0, t.y1, c) + (channel(t.x1, t.y1, c) - channel(t.x0, t.y1, c)) * t.tx;
                const float value = std::clamp(top + (bottom - top) * t.ty, 0.0f, 255.0f);
                packed |= (uint32_t)(value + 0.5f) << (8 * c);
            }
            return packed;
        }

        /// What the unlit and lit shaders read: SampleBilinear when the filter is Linear, Sample when Nearest
        [[nodiscard]] uint32_t SampleFiltered(float u, float v) const
        {
            return options.filter == Common::TextureFilter::Linear ? SampleBilinear(u, v) : Sample(u, v);
        }

        /**
         * The first channel (red), bilinearly filtered, from 0 to 1, as OpenGL's GL_LINEAR filters without
         * mipmaps: texel centres sit at (i + 0.5) / size, and the four texels around the sample point are
         * weighted by distance. Neighbours outside the texture are clamped to the edge or wrapped around,
         * per options.wrap. 1 for a texture without data; a NaN or infinite coordinate reads as 0.
         */
        [[nodiscard]] float SampleFirstChannelBilinear(float u, float v) const
        {
            if (data.empty() || width == 0 || height == 0 || channels == 0) return 1.0f;
            const BilinearTaps t = TapsAt(u, v);

            auto texel = [&](int x, int y)
            {
                return (float)data[((size_t)y * width + (size_t)x) * channels];
            };
            const float top = texel(t.x0, t.y0) + (texel(t.x1, t.y0) - texel(t.x0, t.y0)) * t.tx;
            const float bottom = texel(t.x0, t.y1) + (texel(t.x1, t.y1) - texel(t.x0, t.y1)) * t.tx;
            return (top + (bottom - top) * t.ty) * (1.0f / 255.0f);
        }

    private:
        /// The four texels around a sample point and the weights between them
        struct BilinearTaps
        {
            int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
            float tx = 0.0f, ty = 0.0f;
        };

        /// The bilinear footprint of (u, v): texel centres at (i + 0.5) / size, neighbours clamped or wrapped
        /// per options.wrap. Requires a texture with data.
        [[nodiscard]] BilinearTaps TapsAt(float u, float v) const
        {
            if (!std::isfinite(u)) u = 0.0f; // NaN or inf would make a garbage texel index
            if (!std::isfinite(v)) v = 0.0f;

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

            const int w = (int)width;
            const int h = (int)height;
            auto index = [clampToEdge](int i, int size)
            {
                return clampToEdge ? std::clamp(i, 0, size - 1) : ((i % size) + size) % size;
            };

            BilinearTaps t;
            t.tx = fx - x0f;
            t.ty = fy - y0f;
            t.x0 = index((int)x0f, w);
            t.x1 = index((int)x0f + 1, w);
            t.y0 = index((int)y0f, h);
            t.y1 = index((int)y0f + 1, h);
            return t;
        }
    };
}
