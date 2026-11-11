#pragma once

#include <array>

namespace Renderer::Common
{
    /**
     * The unlit shader's per-material colour effects (effects design, stage E1): `uSaturation` and `uFlash`. Every backend
     * applies them, in this order, to the colour after texture x albedo x vertex colour and the alpha test, and before the
     * stage dim and the screen tint:
     *
     *   saturation  1 leaves the colour as it is, 0 makes it the grey of its own luminance (Rec. 601, the weights
     *               RenderState's dim and the screen tint use); in between mixes, above 1 pushes the colour away from grey
     *               (the result is clamped when it is written). rgb = mix(vec3(luma(rgb)), rgb, saturation)
     *   flash       a colour mixed over the result by its own alpha: rgb = mix(rgb, flash.rgb, flash.a). Alpha is not
     *               changed, so the shape stays the same. Alpha 0 (the default) is no flash
     *
     * This is the reference the GLSL, WGSL and software versions follow (tests compare against it). Each step is skipped at
     * its default, so a material that never sets them draws exactly what it drew before.
     */
    struct ColorEffects
    {
        static constexpr float LumaR = 0.299f;
        static constexpr float LumaG = 0.587f;
        static constexpr float LumaB = 0.114f;

        float saturation = 1.0f;
        std::array<float, 4> flash{0.0f, 0.0f, 0.0f, 0.0f};

        [[nodiscard]] constexpr bool IsDefault() const { return saturation == 1.0f && flash[3] <= 0.0f; }

        /// Applies both effects to one colour (0..1 components; alpha is not touched)
        constexpr void Apply(float &r, float &g, float &b) const
        {
            if (saturation != 1.0f)
            {
                const float luma = LumaR * r + LumaG * g + LumaB * b;
                r = luma + (r - luma) * saturation;
                g = luma + (g - luma) * saturation;
                b = luma + (b - luma) * saturation;
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
