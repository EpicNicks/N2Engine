#include "renderer/common/PostProcess.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// The CPU post-processing (the software renderer's, and the reference of the GPU post shaders: renderer/shaders/vulkan/post.frag,
// renderer/shaders/webgpu/post.wgsl and the OpenGL renderer's inline GLSL). Sampling is OpenGL's GL_LINEAR without mipmaps,
// clamped to the edge: texel centres at (i + 0.5) / size.
namespace Renderer::Common
{
    namespace
    {
        float Finite(const float value, const float fallback)
        {
            return std::isfinite(value) ? value : fallback;
        }

        /// A float RGB image, three floats per pixel, row by row
        struct RgbImage
        {
            int width = 0;
            int height = 0;
            std::vector<float> rgb;

            [[nodiscard]] const float *At(const int x, const int y) const
            {
                return rgb.data() + (static_cast<std::size_t>(y) * width + x) * 3;
            }
        };

        /// GL_LINEAR at (u, v), clamped to the edge
        void Bilinear(const RgbImage &image, const float u, const float v, float out[3])
        {
            const float fx = std::clamp(u * static_cast<float>(image.width) - 0.5f, 0.0f, static_cast<float>(image.width - 1));
            const float fy = std::clamp(v * static_cast<float>(image.height) - 0.5f, 0.0f, static_cast<float>(image.height - 1));
            const int x0 = static_cast<int>(fx);
            const int y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, image.width - 1);
            const int y1 = std::min(y0 + 1, image.height - 1);
            const float tx = fx - static_cast<float>(x0);
            const float ty = fy - static_cast<float>(y0);
            const float *a = image.At(x0, y0);
            const float *b = image.At(x1, y0);
            const float *c = image.At(x0, y1);
            const float *d = image.At(x1, y1);
            for (int i = 0; i < 3; ++i)
            {
                const float top = a[i] + (b[i] - a[i]) * tx;
                const float bottom = c[i] + (d[i] - c[i]) * tx;
                out[i] = top + (bottom - top) * ty;
            }
        }

        /// One 9-tap Gaussian pass along x (dx = spread, dy = 0) or y, taps `spread` texels apart, clamped to the edge
        void Blur(const RgbImage &source, RgbImage &target, const int dx, const int dy)
        {
            for (int y = 0; y < source.height; ++y)
            {
                for (int x = 0; x < source.width; ++x)
                {
                    float sum[3];
                    const float *centre = source.At(x, y);
                    for (int i = 0; i < 3; ++i)
                    {
                        sum[i] = centre[i] * BloomBlurWeights[0];
                    }
                    for (int k = 1; k < 5; ++k)
                    {
                        const float *after = source.At(std::clamp(x + dx * k, 0, source.width - 1),
                                                       std::clamp(y + dy * k, 0, source.height - 1));
                        const float *before = source.At(std::clamp(x - dx * k, 0, source.width - 1),
                                                        std::clamp(y - dy * k, 0, source.height - 1));
                        for (int i = 0; i < 3; ++i)
                        {
                            sum[i] += (after[i] + before[i]) * BloomBlurWeights[k];
                        }
                    }
                    float *out = target.rgb.data() + (static_cast<std::size_t>(y) * target.width + x) * 3;
                    out[0] = sum[0];
                    out[1] = sum[1];
                    out[2] = sum[2];
                }
            }
        }

        /// `iterations` pairs of Gaussian passes, across and then down, each spread one texel further than the last
        void BlurPasses(RgbImage &image, const int iterations)
        {
            RgbImage temp{image.width, image.height, std::vector<float>(image.rgb.size())};
            for (int pass = 0; pass < iterations; ++pass)
            {
                Blur(image, temp, pass + 1, 0);
                Blur(temp, image, 0, pass + 1);
            }
        }

        /// The image at half size: a bilinear read at each half-resolution pixel centre (the average of the 2x2 block under
        /// it)
        RgbImage HalfSize(const RgbImage &source)
        {
            RgbImage half;
            half.width = static_cast<int>(BloomExtent(static_cast<std::uint32_t>(source.width)));
            half.height = static_cast<int>(BloomExtent(static_cast<std::uint32_t>(source.height)));
            half.rgb.resize(static_cast<std::size_t>(half.width) * half.height * 3);
            for (int y = 0; y < half.height; ++y)
            {
                for (int x = 0; x < half.width; ++x)
                {
                    Bilinear(source, (static_cast<float>(x) + 0.5f) / static_cast<float>(half.width),
                             (static_cast<float>(y) + 0.5f) / static_cast<float>(half.height),
                             half.rgb.data() + (static_cast<std::size_t>(y) * half.width + x) * 3);
                }
            }
            return half;
        }

        RgbImage ReadPixels(const std::uint32_t *pixels, const int width, const int height)
        {
            const std::size_t count = static_cast<std::size_t>(width) * height;
            RgbImage image{width, height, std::vector<float>(count * 3)};
            for (std::size_t i = 0; i < count; ++i)
            {
                const std::uint32_t packed = pixels[i];
                image.rgb[i * 3 + 0] = static_cast<float>(packed & 0xFFu) / 255.0f;
                image.rgb[i * 3 + 1] = static_cast<float>((packed >> 8) & 0xFFu) / 255.0f;
                image.rgb[i * 3 + 2] = static_cast<float>((packed >> 16) & 0xFFu) / 255.0f;
            }
            return image;
        }

        /// The blur stage, in place: each pixel mixed towards the half-size blurred image
        void BlurFrame(std::uint32_t *pixels, const int width, const int height, const PostProcessSettings &settings)
        {
            const RgbImage scene = ReadPixels(pixels, width, height);
            RgbImage blurred = HalfSize(scene);
            BlurPasses(blurred, settings.blurIterations);
            for (int y = 0; y < height; ++y)
            {
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
                for (int x = 0; x < width; ++x)
                {
                    float soft[3];
                    Bilinear(blurred, (static_cast<float>(x) + 0.5f) / static_cast<float>(width), v, soft);
                    const float *sharp = scene.At(x, y);
                    const std::size_t index = static_cast<std::size_t>(y) * width + x;
                    std::uint32_t packed = pixels[index] & 0xFF000000u;
                    for (int i = 0; i < 3; ++i)
                    {
                        const float value =
                            std::clamp(sharp[i] + (soft[i] - sharp[i]) * settings.blurStrength, 0.0f, 1.0f);
                        packed |= static_cast<std::uint32_t>(value * 255.0f + 0.5f) << (8 * i);
                    }
                    pixels[index] = packed;
                }
            }
        }
    }

    PostProcessSettings PostProcessSettings::Sanitized(const PostProcessSettings &settings)
    {
        const PostProcessSettings defaults;
        PostProcessSettings result = settings;
        result.bloomThreshold = std::clamp(Finite(settings.bloomThreshold, defaults.bloomThreshold), 0.0f, 1.0f);
        result.bloomIntensity = std::clamp(Finite(settings.bloomIntensity, defaults.bloomIntensity), 0.0f, MaxBloomIntensity);
        result.bloomIterations = std::clamp(settings.bloomIterations, 1, MaxBloomIterations);
        for (int i = 0; i < 3; ++i)
        {
            result.bloomTint[i] = std::clamp(Finite(settings.bloomTint[i], 1.0f), 0.0f, MaxBloomIntensity);
        }
        result.invert = std::clamp(Finite(settings.invert, 0.0f), 0.0f, 1.0f);
        result.chromaticAberration = std::clamp(Finite(settings.chromaticAberration, 0.0f), 0.0f, MaxChromaticAberration);
        result.vignette = std::clamp(Finite(settings.vignette, 0.0f), 0.0f, 1.0f);
        result.blurStrength = std::clamp(Finite(settings.blurStrength, defaults.blurStrength), 0.0f, 1.0f);
        result.blurIterations = std::clamp(settings.blurIterations, 1, MaxBlurIterations);
        return result;
    }

    PostProcessParams PostProcessParams::For(const PostPassMode mode, const PostProcessSettings &settings)
    {
        PostProcessParams params;
        params.a[0] = static_cast<float>(mode);
        params.a[1] = settings.bloomThreshold;
        params.a[2] = settings.BloomActive() ? settings.bloomIntensity : 0.0f;
        params.a[3] = settings.invert;
        params.b[2] = settings.chromaticAberration;
        params.b[3] = settings.vignette;
        params.c[0] = settings.bloomTint[0];
        params.c[1] = settings.bloomTint[1];
        params.c[2] = settings.bloomTint[2];
        if (mode == PostPassMode::Mix)
        {
            params.a[2] = settings.blurStrength;
        }
        return params;
    }

    void ApplyPostProcessCpu(std::uint32_t *pixels, const int width, const int height, const PostProcessSettings &input)
    {
        if (pixels == nullptr || width <= 0 || height <= 0)
        {
            return;
        }
        const PostProcessSettings settings = PostProcessSettings::Sanitized(input);
        if (!settings.IsActive())
        {
            return;
        }

        // The blur first, written back as the GPU passes write the frame; the other effects read its result
        if (settings.BlurActive())
        {
            BlurFrame(pixels, width, height, settings);
            if (!settings.CompositeActive())
            {
                return;
            }
        }

        const RgbImage scene = ReadPixels(pixels, width, height);

        // Bloom: the bright pass at half resolution (a bilinear read at each half-resolution pixel centre: the average of the
        // 2x2 block under it), then the blur passes, across and then down, each spread one texel further than the last
        RgbImage bloom;
        const bool bloomOn = settings.BloomActive();
        if (bloomOn)
        {
            bloom.width = static_cast<int>(BloomExtent(static_cast<std::uint32_t>(width)));
            bloom.height = static_cast<int>(BloomExtent(static_cast<std::uint32_t>(height)));
            bloom.rgb.resize(static_cast<std::size_t>(bloom.width) * bloom.height * 3);
            for (int y = 0; y < bloom.height; ++y)
            {
                for (int x = 0; x < bloom.width; ++x)
                {
                    float c[3];
                    Bilinear(scene, (static_cast<float>(x) + 0.5f) / static_cast<float>(bloom.width),
                             (static_cast<float>(y) + 0.5f) / static_cast<float>(bloom.height), c);
                    const float brightest = std::max({c[0], c[1], c[2]});
                    const float weight = std::max(brightest - settings.bloomThreshold, 0.0f) / std::max(brightest, 1e-4f);
                    float *out = bloom.rgb.data() + (static_cast<std::size_t>(y) * bloom.width + x) * 3;
                    out[0] = c[0] * weight;
                    out[1] = c[1] * weight;
                    out[2] = c[2] * weight;
                }
            }
            BlurPasses(bloom, settings.bloomIterations);
        }

        // The composite, at each pixel centre
        const float intensity = bloomOn ? settings.bloomIntensity : 0.0f;
        for (int y = 0; y < height; ++y)
        {
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
            for (int x = 0; x < width; ++x)
            {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                const float *base = scene.At(x, y);
                float rgb[3] = {base[0], base[1], base[2]};
                if (settings.chromaticAberration > 0.0f)
                {
                    const float du = (u - 0.5f) * settings.chromaticAberration;
                    const float dv = (v - 0.5f) * settings.chromaticAberration;
                    float shifted[3];
                    Bilinear(scene, u + du, v + dv, shifted);
                    rgb[0] = shifted[0];
                    Bilinear(scene, u - du, v - dv, shifted);
                    rgb[2] = shifted[2];
                }
                if (bloomOn)
                {
                    float glow[3];
                    Bilinear(bloom, u, v, glow);
                    for (int i = 0; i < 3; ++i)
                    {
                        rgb[i] += glow[i] * intensity * settings.bloomTint[i];
                    }
                }
                const float du = u - 0.5f;
                const float dv = v - 0.5f;
                const float distance = std::sqrt(du * du + dv * dv) * 1.41421356f;
                const float t = std::clamp((distance - VignetteStart) / (1.0f - VignetteStart), 0.0f, 1.0f);
                const float darken = 1.0f - settings.vignette * (t * t * (3.0f - 2.0f * t));
                std::uint32_t packed = pixels[index] & 0xFF000000u;
                for (int i = 0; i < 3; ++i)
                {
                    float value = std::clamp(rgb[i], 0.0f, 1.0f);
                    value = value + (1.0f - 2.0f * value) * settings.invert;
                    value = std::clamp(value * darken, 0.0f, 1.0f);
                    packed |= static_cast<std::uint32_t>(value * 255.0f + 0.5f) << (8 * i);
                }
                pixels[index] = packed;
            }
        }
    }
}
