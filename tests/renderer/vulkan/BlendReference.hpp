#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "CrossBackendDiff.hpp"

// The GL-semantics REFERENCE for blended Vulkan frames (#43, stage D). The software renderer cannot blend (it draws every
// blended draw opaque), so a blended Vulkan frame must never be compared with one software frame: that would encode
// Software-only behaviour. Two things are done instead, and both are device-free (this header needs no Vulkan device; its
// helpers are tested in BlendReferenceTests.cpp):
//
//  1. COMPOSITE. Each blended draw ("layer") is rendered on the software renderer OPAQUE, alone, on the clear colour: that
//     gives its colour per pixel and, by comparing with the clear colour (or with a flat render of the same geometry), the
//     pixels it covers. The reference frame is then built on the CPU with OpenGL's blend equation, layer by layer in
//     submission order, over the software frame of everything behind it:
//         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), equation ADD, the SAME factors for colour and alpha:
//             out.rgb = round(a * src.rgb + (1 - a) * dst.rgb)           (255 units, clamped to 0..255)
//             out.a   = round(255 * a * a + (1 - a) * dst.a)             (the source alpha is a, so a * a)
//         additive, glBlendFunc(GL_SRC_ALPHA, GL_ONE):
//             out.rgb = min(255, round(a * src.rgb + dst.rgb))
//             out.a   = min(255, round(255 * a * a + dst.a))
//     Diff::Compare/Agree then compares the Vulkan frame with the composite: STRICT (2/255) for nearest-sampled unlit scenes,
//     LIT (mean <= 2/255, <= 1% over 8/255, a 1-pixel coverage-edge border, an exclusion cap) for linear-filtered ones. The
//     composite's own rounding is within the 2/255 of the strict mode (the GPU blends in float and rounds once); no extra
//     allowance is made.
//
//  2. OPAQUE MASK. Where a scene's layers cannot be separated (an engine scene of many renderables), the strict diff runs on
//     the OPAQUE parts only: the pixels no blended draw covers (grown by a border so rasterisation tie rules at the blended
//     edges do not count) are compared with the software frame (Diff::Options::ignore), and the blended parts get
//     per-backend STRUCTURAL expectations at interior pixels: the exact GL-equation colour (+-2). The blended coverage comes
//     from the same scene with every blended draw made opaque (or from a flat render of the blended geometry).
//
// Known divergence, not modelled here: a blended LIT draw in LINEAR lighting. OpenGL blends it in linear space (its
// framebuffer is sRGB with GL_FRAMEBUFFER_SRGB on for that draw); Vulkan's target is UNORM and the lit shader encodes in
// the shader, so it blends the encoded values (like every unlit, text, UI, particle and Gamma-lit draw, which match GL).

namespace VulkanTesting::Blend
{
    using Diff::Image;

    /// One blended draw: what the software renderer drew for it OPAQUE, alone on the clear colour, and where
    struct Layer
    {
        Image colour;              // the layer's colour where it is covered (anything elsewhere)
        std::vector<bool> covered; // the pixels the layer covers, one flag per pixel, row 0 at the bottom
        double alpha = 1.0;        // the source alpha of every covered pixel, as the shader outputs it (0..1)
        bool additive = false;     // glBlendFunc(GL_SRC_ALPHA, GL_ONE) instead of GL_ONE_MINUS_SRC_ALPHA
    };

    /// A value in 255 units to a UNORM byte: rounded to the nearest, clamped
    inline std::uint8_t ToByte(const double value)
    {
        const long rounded = std::lround(value);
        return static_cast<std::uint8_t>(std::clamp(rounded, 0L, 255L));
    }

    /// One colour channel of a blend: `source` and `destination` in 0..255, `alpha` the source alpha in 0..1
    inline std::uint8_t BlendChannel(const std::uint8_t source, const std::uint8_t destination, const double alpha,
                                     const bool additive)
    {
        const double value = additive ? alpha * source + destination : alpha * source + (1.0 - alpha) * destination;
        return ToByte(value);
    }

    /// The alpha channel of a blend: GL uses the same factors for alpha, and the source alpha is `alpha`
    inline std::uint8_t BlendAlpha(const double alpha, const std::uint8_t destinationAlpha, const bool additive)
    {
        const double value = additive ? 255.0 * alpha * alpha + destinationAlpha
                                      : 255.0 * alpha * alpha + (1.0 - alpha) * destinationAlpha;
        return ToByte(value);
    }

    /// The pixels of `image` that are not `background` (rgb), one flag per pixel: what a flat render of a draw covers
    inline std::vector<bool> CoveredPixels(const Image& image, const std::array<std::uint8_t, 3>& background)
    {
        std::vector<bool> covered(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height), false);
        for (int y = 0; y < image.height; ++y)
        {
            for (int x = 0; x < image.width; ++x)
            {
                const std::uint8_t* p = image.Pixel(x, y);
                covered[static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)] =
                    p[0] != background[0] || p[1] != background[1] || p[2] != background[2];
            }
        }
        return covered;
    }

    /// `mask` grown by `border` pixels in every direction (the 8-neighbourhood, repeated): the pixels near a blended edge
    inline std::vector<bool> Dilate(const std::vector<bool>& mask, const int width, const int height, const int border)
    {
        std::vector<bool> grown = mask;
        for (int pass = 0; pass < border; ++pass)
        {
            const std::vector<bool> before = grown;
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    if (!before[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)])
                    {
                        continue;
                    }
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int nx = x + dx;
                            const int ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < width && ny < height)
                            {
                                grown[static_cast<std::size_t>(ny) * static_cast<std::size_t>(width) +
                                      static_cast<std::size_t>(nx)] = true;
                            }
                        }
                    }
                }
            }
        }
        return grown;
    }

    /// The union of two masks of one size
    inline std::vector<bool> Union(const std::vector<bool>& a, const std::vector<bool>& b)
    {
        std::vector<bool> both = a;
        for (std::size_t i = 0; i < both.size() && i < b.size(); ++i)
        {
            both[i] = both[i] || b[i];
        }
        return both;
    }

    /// `under` with `layer` blended over it as OpenGL would. A layer whose size does not match is not applied (the
    /// returned image is `under`; the comparison that follows then fails loudly rather than pass on nothing)
    inline Image Composite(const Image& under, const Layer& layer)
    {
        Image out = under;
        const std::size_t pixels = static_cast<std::size_t>(under.width) * static_cast<std::size_t>(under.height);
        if (layer.colour.width != under.width || layer.colour.height != under.height || layer.covered.size() != pixels)
        {
            return out;
        }
        for (int y = 0; y < under.height; ++y)
        {
            for (int x = 0; x < under.width; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(under.width) +
                    static_cast<std::size_t>(x);
                if (!layer.covered[index])
                {
                    continue;
                }
                const std::uint8_t* source = layer.colour.Pixel(x, y);
                std::uint8_t* destination = &out.rgba[index * 4];
                for (int channel = 0; channel < 3; ++channel)
                {
                    destination[channel] = BlendChannel(source[channel], destination[channel], layer.alpha, layer.additive);
                }
                destination[3] = BlendAlpha(layer.alpha, destination[3], layer.additive);
            }
        }
        return out;
    }

    /// `under` with each layer blended over it in turn (submission order)
    inline Image Composite(const Image& under, const std::vector<Layer>& layers)
    {
        Image out = under;
        for (const Layer& layer : layers)
        {
            out = Composite(out, layer);
        }
        return out;
    }
}
