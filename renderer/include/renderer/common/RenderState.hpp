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
     * The defaults are the state every draw had before render state existed: depth test and depth write
     * on, back faces culled, blending on (the OpenGL backend has always blended with
     * SRC_ALPHA, ONE_MINUS_SRC_ALPHA; a fragment with alpha 1 is unaffected).
     */
    struct RenderState
    {
        bool depthTest = true;
        bool depthWrite = true;
        CullMode cull = CullMode::Back;
        /// Alpha blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA). The software renderer has no blending and
        /// ignores this flag: every software draw is opaque.
        bool blend = true;

        /// The state the Opaque render queue draws with (the defaults).
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
