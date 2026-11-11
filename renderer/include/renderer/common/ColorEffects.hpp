#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "renderer/common/RenderState.hpp"

namespace Renderer::Common
{
    /**
     * The unlit shader's per-material colour effects (effects design, stage E1): `uSaturation` and `uFlash`. Every backend
     * applies them, in this order, to the colour after texture x albedo x vertex colour and the alpha test, and before the
     * stage dim and the screen tint:
     *
     *   saturation  1 leaves the colour as it is, 0 makes it the grey of its own luminance (Rec. 601, RenderState's dim
     *               weights, which the screen tint uses too); in between mixes, above 1 pushes the colour away from grey.
     *               rgb = clamp(mix(vec3(luma(rgb)), rgb, saturation), 0, 1): clamped at once, so the dim and the tint
     *               after it see the same in-range colour on every backend (the software renderer packs to bytes before
     *               them)
     *   flash       a colour mixed over the result by its own alpha: rgb = mix(rgb, flash.rgb, flash.a). Alpha is not
     *               changed, so the shape stays the same. Alpha 0 (the default) is no flash
     *
     * This is the reference the GLSL, WGSL and software versions follow (tests compare against it). Each step is skipped at
     * its default, so a material that never sets them draws exactly what it drew before. ClampSaturation and ClampFlash give the
     * values the engine stores and sends (saturation 0..4, a non-finite one 1; each flash component 0..1, a non-finite one 0).
     */
    struct ColorEffects
    {
        static constexpr float MaxSaturation = 4.0f;

        float saturation = 1.0f;
        std::array<float, 4> flash{0.0f, 0.0f, 0.0f, 0.0f};

        [[nodiscard]] constexpr bool IsDefault() const { return saturation == 1.0f && flash[3] <= 0.0f; }

        [[nodiscard]] static float ClampSaturation(const float value)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0f, MaxSaturation) : 1.0f;
        }

        [[nodiscard]] static float ClampFlashComponent(const float value)
        {
            return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
        }

        /// A flash colour with each component in its range, for a colour type with r, g, b, a members and an (r, g, b, a)
        /// constructor (the engine's Common::Color)
        template <class Colour> [[nodiscard]] static Colour ClampFlash(const Colour &colour)
        {
            return Colour{ClampFlashComponent(colour.r), ClampFlashComponent(colour.g), ClampFlashComponent(colour.b),
                          ClampFlashComponent(colour.a)};
        }

        /// Applies both effects to one colour (0..1 components; alpha is not touched)
        constexpr void Apply(float &r, float &g, float &b) const
        {
            if (saturation != 1.0f)
            {
                const float luma = RenderState::DimLumaR * r + RenderState::DimLumaG * g + RenderState::DimLumaB * b;
                r = std::clamp(luma + (r - luma) * saturation, 0.0f, 1.0f);
                g = std::clamp(luma + (g - luma) * saturation, 0.0f, 1.0f);
                b = std::clamp(luma + (b - luma) * saturation, 0.0f, 1.0f);
            }
            if (flash[3] > 0.0f)
            {
                const float amount = flash[3] < 1.0f ? flash[3] : 1.0f;
                r += (flash[0] - r) * amount;
                g += (flash[1] - g) * amount;
                b += (flash[2] - b) * amount;
            }
        }
    };
}
