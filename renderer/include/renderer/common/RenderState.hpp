#pragma once

#include <cstdint>

namespace Renderer::Common
{
    /// Which triangle faces a draw discards. Front faces wind counter-clockwise (the OpenGL default).
    enum class CullMode : std::uint8_t
    {
        None,
        Back,
        Front
    };

    /**
     * The fixed-function state one draw is made with. It travels with the draw
     * (IRenderer::DrawMesh(mesh, model, material, state)) instead of being set on the renderer, because a
     * backend may reorder draws (the software renderer does) and a global setter could not follow them.
     *
     * The defaults are the Opaque render queue's state: depth test and depth write on, back faces culled,
     * blending off. (Before #3 P2 blending was on by default, so an opaque draw with a translucent colour
     * blended; now only draws that ask for it blend: the Transparent queue, a Blend material, the UI pass.)
     */
    struct RenderState
    {
        bool depthTest = true;
        bool depthWrite = true;
        CullMode cull = CullMode::Back;
        /// Alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA). The software renderer has no blending and
        /// ignores this flag: every software draw is opaque. The Vulkan renderer blends exactly like OpenGL.
        bool blend = false;
        /// With blend: add the source (SRC_ALPHA, ONE) instead of mixing it with what is behind (fire, sparks).
        /// The software renderer, which can't blend at all, ignores it too.
        bool additive = false;
        /**
         * How much the draw is dimmed, 0 (not at all, the default) to 1: the colour it would write is mixed towards a
         * darkened grey of its own luminance, rgb = mix(rgb, luma(rgb) * DimGrey, dim), alpha unchanged (StageDimmed).
         * The editor's prefab stage draws the scene around a prefab with it (In Scene mode), so the scene stays readable
         * but is clearly not what is being edited. It travels with the draw like the rest of this state; a renderer
         * applies IRenderer::SetStageDim's value to every draw recorded while it is set (IRenderer::WithStageDim).
         * Every backend's built-in shaders (unlit, lit, text) apply it; a material with a custom OpenGL shader that does
         * not declare `uniform float uStageDim` is drawn undimmed.
         */
        float dim = 0.0f;

        /// The luminance weights and the grey a fully dimmed colour becomes (times its luminance)
        static constexpr float DimLumaR = 0.299f;
        static constexpr float DimLumaG = 0.587f;
        static constexpr float DimLumaB = 0.114f;
        static constexpr float DimGrey = 0.45f;

        /// The dim maths every backend's shaders implement, on one colour (0..1 components; alpha is not touched)
        static constexpr void StageDimmed(float &r, float &g, float &b, const float dim)
        {
            if (dim <= 0.0f)
            {
                return;
            }
            const float amount = dim < 1.0f ? dim : 1.0f;
            const float grey = (DimLumaR * r + DimLumaG * g + DimLumaB * b) * DimGrey;
            r += (grey - r) * amount;
            g += (grey - g) * amount;
            b += (grey - b) * amount;
        }

        /// The state the Opaque render queue draws with (the defaults): depth-tested and written, not blended.
        /// Opaque and alpha-mask (cutout) materials draw with it.
        [[nodiscard]] static constexpr RenderState Opaque() { return RenderState{}; }

        /// The state the Transparent render queue draws with: blended, depth-tested against the opaque
        /// geometry, but not writing depth, so transparent surfaces behind one another all show.
        [[nodiscard]] static constexpr RenderState Transparent()
        {
            RenderState state;
            state.depthWrite = false;
            state.blend = true;
            return state;
        }

        friend constexpr bool operator==(const RenderState &, const RenderState &) = default;
    };
}
