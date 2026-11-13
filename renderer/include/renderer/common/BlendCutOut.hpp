#pragma once

#include <algorithm>
#include <cmath>

namespace Renderer::Common
{
    /// The smallest cut-out BlendCutOutBelow returns for a textured draw: half of one 8-bit alpha step, so a colour alpha
    /// under 1/255 cuts out every pixel (a test against 0 would keep them all and draw the quad as a box)
    inline constexpr float MinBlendCutOut = 0.5f / 255.0f;

    /**
     * The alpha test that stands in for blending on a renderer that cannot blend (IRenderer::SupportsBlending false: the
     * software renderer, also the web player's fallback), for a textured draw whose final alpha is the texture's alpha times
     * the colour's. A pixel under half the colour's alpha (a texel under half opaque) is not drawn, so a texture's
     * transparent parts are cut out instead of drawn as boxes, and fading the colour does not make the opaque texels vanish
     * early. A colour alpha of 0 cuts out every pixel (MinBlendCutOut). 0 (no test) for an untextured draw and wherever the
     * renderer blends. A non-finite alpha counts as 1 here; the shader still multiplies by it, so a non-finite colour is
     * not made safe by this. Sprite2D and the UI Image send it as uAlphaCutoff.
     */
    [[nodiscard]] inline float BlendCutOutBelow(const bool textured, const bool rendererBlends, const float colorAlpha)
    {
        if (!textured || rendererBlends)
        {
            return 0.0f;
        }
        const float alpha = std::isfinite(colorAlpha) ? std::clamp(colorAlpha, 0.0f, 1.0f) : 1.0f;
        return std::max(0.5f * alpha, MinBlendCutOut);
    }
}
