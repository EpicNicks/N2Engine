#pragma once

#include <cstdint>

#include "renderer/common/TextureOptions.hpp"

namespace Renderer::Common
{
    /**
     * The full-screen effects IRenderer::ApplyPostProcess runs over what a frame has drawn so far (issue #445; the engine runs
     * it after the scene and before the screen-space UI, so the HUD is never bloomed, as in Unity). In the order they apply:
     *
     * 1. Chromatic aberration: the red channel is read from a point moved away from the screen centre and blue from one moved
     *    towards it, by `chromaticAberration` times the distance from the centre (0.02 is a strong fringe at the corners).
     * 2. Bloom: the parts of the image brighter than `bloomThreshold` (the brightest channel, 0..1) are taken at half
     *    resolution, blurred `bloomIterations` times (each pass a 9-tap Gaussian across and then down, its taps spread one
     *    texel further than the last pass's, so the glow widens with every pass), and added back times `bloomIntensity` and
     *    `bloomTint`. The sum is clamped to 0..1: frames are 8-bit, so the threshold is what picks out the "bright" parts.
     * 3. Invert: the colour mixed towards 1 - colour by `invert` (1 is a full negative).
     * 4. Vignette: the colour darkened towards the corners, by up to `vignette` (0..1) at them, from 0.3 of the way out.
     *
     * Alpha is the frame's, untouched. Every GPU backend (OpenGL, Vulkan, WebGPU) and the software renderer (ApplyPostProcessCpu)
     * compute the same thing; the GPU ones keep the half-resolution bloom images in 8 bits, the CPU one in floats.
     */
    struct PostProcessSettings
    {
        static constexpr int MaxBloomIterations = 8;
        static constexpr float MaxBloomIntensity = 16.0f;
        static constexpr float MaxChromaticAberration = 0.25f;
        static constexpr int MaxBlurIterations = 8;

        bool bloom = false;
        /// The brightness (the largest of r, g and b) above which a pixel glows: 0 makes everything glow, 1 nothing
        float bloomThreshold = 0.7f;
        /// How much of the blurred glow is added back (0 to MaxBloomIntensity)
        float bloomIntensity = 1.0f;
        /// Blur passes, 1 to MaxBloomIterations: the glow's radius grows with each (a cost of two half-resolution passes each)
        int bloomIterations = 4;
        /// The glow's colour multiplier (each 0 to MaxBloomIntensity)
        float bloomTint[3] = {1.0f, 1.0f, 1.0f};
        /// 0 (none) to 1 (a full negative)
        float invert = 0.0f;
        /// 0 (none) to MaxChromaticAberration: the red/blue offset as a fraction of the distance from the screen centre
        float chromaticAberration = 0.0f;
        /// 0 (none) to 1 (black corners)
        float vignette = 0.0f;
        /// A blur of the whole scene, applied before every other effect (issue #445)
        bool blur = false;
        /// 0 (the sharp image) to 1 (the blurred one): a mix of the two
        float blurStrength = 1.0f;
        /// Blur passes, 1 to MaxBlurIterations: the radius grows with each (a cost of two half-resolution passes each)
        int blurIterations = 3;

        /// Every value clamped to its range; a value that is not finite takes its default (bloom keeps its flag)
        [[nodiscard]] static PostProcessSettings Sanitized(const PostProcessSettings &settings);
        /// Bloom is on and adds something
        [[nodiscard]] bool BloomActive() const { return bloom && bloomIntensity > 0.0f && bloomThreshold < 1.0f; }
        /// The blur is on and shows
        [[nodiscard]] bool BlurActive() const { return blur && blurStrength > 0.0f; }
        /// An effect of the composite pass (everything but the blur) changes the image
        [[nodiscard]] bool CompositeActive() const
        {
            return BloomActive() || invert > 0.0f || chromaticAberration > 0.0f || vignette > 0.0f;
        }
        /// Any effect changes the image (a frame with none skips the passes)
        [[nodiscard]] bool IsActive() const { return BlurActive() || CompositeActive(); }
    };

    /// What one pass of the GPU post shaders does (PostProcessParams::mode)
    enum class PostPassMode : std::uint8_t
    {
        Copy = 0,      ///< the source's colour with alpha 1 (a render-texture capture through a draw: WebGPU)
        Bright = 1,    ///< the source above the threshold (the bloom's first, half-resolution, pass)
        Blur = 2,      ///< a 9-tap Gaussian along (stepX, stepY)
        Composite = 3, ///< chromatic aberration, bloom added, invert, vignette: the pass that writes the frame
        Mix = 4,       ///< from the source towards the second image by `intensity`, keeping the source's alpha: the blur
    };

    /// The bloom blur's taps: weight 0 at the centre, 1-4 on each side (they sum to 1). The GPU shaders hold the same numbers
    inline constexpr float BloomBlurWeights[5] = {0.2270270270f, 0.1945945946f, 0.1216216216f, 0.0540540541f, 0.0162162162f};
    /// The vignette starts darkening this far out (0 the centre, 1 a corner)
    inline constexpr float VignetteStart = 0.3f;

    /**
     * The block every GPU post shader reads (48 bytes: three vec4, the same in std140, push constants and WGSL):
     * a = (mode, threshold, intensity, invert), b = (stepX, stepY, chromaticAberration, vignette); for Mix the intensity
     * is the blur's strength.
     * c = (tint r, tint g, tint b, flipY). stepX/stepY are the blur direction in uv of the source (one texel times the
     * pass's spread); flipY 1 makes the vertex stage read the source upside down (a capture into a texture whose first row
     * is the bottom of the picture).
     */
    struct PostProcessParams
    {
        float a[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float c[4] = {1.0f, 1.0f, 1.0f, 0.0f};

        /// The block of one pass of `settings` (taken as it is: sanitize first)
        [[nodiscard]] static PostProcessParams For(PostPassMode mode, const PostProcessSettings &settings);
    };
    static_assert(sizeof(PostProcessParams) == 48);

    /// The size of the bloom's images for a frame of width x height: half, at least 1
    [[nodiscard]] constexpr std::uint32_t BloomExtent(const std::uint32_t frameExtent)
    {
        return frameExtent / 2 > 0 ? frameExtent / 2 : 1;
    }

    /**
     * The order every backend runs the passes in. With the blur active: the frame is copied, drawn at half size (Copy),
     * blurred (blurIterations pairs of Blur passes, as the bloom's), and drawn back over the frame mixed with the sharp
     * copy (Mix). Then, if CompositeActive, the bloom's passes and the composite run on that result.
     */

    /**
     * The software renderer's post-processing, and the reference of the GPU shaders: `settings` (sanitized here) applied in
     * place to `pixels`, width * height packed RGBA8 (red in the low byte). The effects are symmetric, so the row order does
     * not matter. Does nothing for an inactive setting, a null buffer or an empty size.
     */
    void ApplyPostProcessCpu(std::uint32_t *pixels, int width, int height, const PostProcessSettings &settings);

    /// The options a render texture is made with on every backend: linear, clamped to the edge, no mipmaps, not sRGB
    [[nodiscard]] constexpr TextureOptions RenderTextureOptions()
    {
        TextureOptions options;
        options.filter = TextureFilter::Linear;
        options.wrap = TextureWrap::ClampToEdge;
        options.mipmaps = false;
        options.srgb = false;
        return options;
    }

    /// The largest side of a render texture (IRenderer::CreateRenderTexture)
    inline constexpr std::uint32_t MaxRenderTextureSize = 8192;
}
