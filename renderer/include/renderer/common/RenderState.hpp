#pragma once

#include <algorithm>
#include <cmath>
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

    /// Where a ScreenTint's circle is in one render target, in that target's pixels: what the GPU shaders compare
    /// gl_FragCoord (WGSL @builtin(position)) with, and the software renderer the centre of the pixel it writes
    struct ScreenTintPixels
    {
        float centreX = 0.0f;
        float centreY = 0.0f;
        float radius = 0.0f;
    };

    /// What every GPU backend's shaders receive for a ScreenTint in one render target (ScreenTint::ToUniforms): an
    /// inactive tint is saturation 1 (which the shaders skip), inside 0 and a zero circle
    struct ScreenTintUniforms
    {
        float saturation = 1.0f;
        std::int32_t inside = 0;
        ScreenTintPixels circle{};
    };

    /**
     * A frame-scoped saturation change inside (or outside) a circle on the screen: the time-stop and full-screen grey
     * effects (effects design, stage E2). Set on the renderer with IRenderer::SetScreenTint; every draw recorded while it
     * is set carries it in its RenderState (RenderState::tint), as the stage dim travels, so it survives the software
     * renderer's draw reordering. Each built-in shader (unlit, lit, text) on each backend tests its own pixel against
     * the circle and changes that pixel's saturation there.
     *
     * Saturation is a linear map of the colour and alpha blending mixes colours linearly, so tinting each draw and then
     * blending gives the image that tinting the finished frame would give (normal and additive blending alike). The clear
     * colour is not a draw, so no shader tints it: Application tints it on the CPU when the tint covers the whole target
     * (CoversWholeTarget), and otherwise a bare clear colour stays as it is (a background drawn as a sprite is tinted).
     */
    struct ScreenTint
    {
        /// 1 is the colour as drawn (no tint, the default), 0 is grey of the colour's Rec. 601 luma; values between mix.
        /// IRenderer::SetScreenTint clamps it to 0..1
        float saturation = 1.0f;
        /// The circle's centre, 0 to 1 of the render target: (0, 0) the bottom-left corner, (1, 1) the top-right
        float centreX = 0.5f;
        float centreY = 0.5f;
        /// The circle's radius in half screen heights: 1 reaches the top and bottom edges from the middle. The default
        /// covers any screen
        float radius = WholeScreenRadius;
        /// True: the tint applies inside the circle; false: outside it
        bool inside = true;

        /// A radius that covers every corner of any render target from any centre on it
        static constexpr float WholeScreenRadius = 1.0e6f;

        /// `tint` made safe to draw with: saturation clamped to 0..1 and a negative radius to 0; a tint with a value that
        /// is not finite is no tint (ScreenTint{})
        [[nodiscard]] static ScreenTint Sanitized(const ScreenTint &tint)
        {
            if (!std::isfinite(tint.saturation) || !std::isfinite(tint.centreX) || !std::isfinite(tint.centreY) ||
                !std::isfinite(tint.radius))
            {
                return ScreenTint{};
            }
            ScreenTint safe = tint;
            safe.saturation = std::clamp(tint.saturation, 0.0f, 1.0f);
            safe.radius = std::max(tint.radius, 0.0f);
            return safe;
        }

        /// Whether the tint changes anything at all: saturation below 1 and, inside a circle, a circle that has an inside
        [[nodiscard]] constexpr bool IsActive() const
        {
            return saturation < 1.0f && (!inside || radius > 0.0f);
        }

        /// The circle in a target of `width` x `height` pixels. `yDown`: the target's pixel rows count down from the
        /// top (Vulkan and WebGPU framebuffer coordinates); otherwise up from the bottom (OpenGL's gl_FragCoord, the
        /// software renderer's pixel loop)
        [[nodiscard]] constexpr ScreenTintPixels InPixels(const float width, const float height, const bool yDown) const
        {
            ScreenTintPixels pixels;
            pixels.centreX = centreX * width;
            pixels.centreY = yDown ? (1.0f - centreY) * height : centreY * height;
            pixels.radius = radius * 0.5f * height;
            return pixels;
        }

        /// The shader uniforms for this tint in a `width` x `height` target (`yDown` as for InPixels); the one place
        /// the encoding of an inactive tint is decided (OpenGL's uniforms and the Vulkan/WebGPU per-draw block)
        [[nodiscard]] constexpr ScreenTintUniforms ToUniforms(const float width, const float height, const bool yDown) const
        {
            if (!IsActive())
            {
                return ScreenTintUniforms{};
            }
            return ScreenTintUniforms{saturation, inside ? 1 : 0, InPixels(width, height, yDown)};
        }

        /// Whether the pixel whose centre is at (x, y), in the coordinates of `circle`, gets the tint
        [[nodiscard]] constexpr bool Covers(const ScreenTintPixels &circle, const float x, const float y) const
        {
            const float dx = x - circle.centreX;
            const float dy = y - circle.centreY;
            const bool inCircle = dx * dx + dy * dy <= circle.radius * circle.radius;
            return inCircle == inside;
        }

        /// Whether every pixel of a `width` x `height` target gets the tint: inside, all four corners are in the circle;
        /// outside, the circle does not reach the target at all. What decides whether the clear colour is tinted too
        [[nodiscard]] constexpr bool CoversWholeTarget(const float width, const float height) const
        {
            if (!IsActive() || width <= 0.0f || height <= 0.0f)
            {
                return false;
            }
            const ScreenTintPixels circle = InPixels(width, height, false);
            const float r2 = circle.radius * circle.radius;
            if (inside)
            {
                const float left = circle.centreX;
                const float right = width - circle.centreX;
                const float bottom = circle.centreY;
                const float top = height - circle.centreY;
                const float farX = left > right ? left : right;
                const float farY = bottom > top ? bottom : top;
                return farX * farX + farY * farY <= r2;
            }
            // The point of the target nearest the centre, no nearer than the radius (a circle of radius 0 has no inside)
            const float nearX = circle.centreX < 0.0f ? 0.0f : (circle.centreX > width ? width : circle.centreX);
            const float nearY = circle.centreY < 0.0f ? 0.0f : (circle.centreY > height ? height : circle.centreY);
            const float dx = nearX - circle.centreX;
            const float dy = nearY - circle.centreY;
            return dx * dx + dy * dy >= r2;
        }

        /// The saturation maths every backend's shaders implement, on one colour (0..1 components; alpha is not
        /// touched): rgb = mix(vec3(luma(rgb)), rgb, saturation), with RenderState's luma weights
        static constexpr void Saturated(float &r, float &g, float &b, const float saturation)
        {
            const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;
        }

        friend constexpr bool operator==(const ScreenTint &, const ScreenTint &) = default;
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
        /**
         * The screen tint the draw is made with (ScreenTint; the default changes nothing). A renderer applies
         * IRenderer::SetScreenTint's value to every draw recorded while it is set (IRenderer::WithFrameModifiers), and
         * every backend's built-in shaders apply it after the dim; a material with a custom OpenGL shader is not tinted.
         * Not part of any pipeline key: it reaches the shaders as per-draw uniforms.
         */
        ScreenTint tint{};

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
