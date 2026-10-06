#pragma once

#include <math/Vector2.hpp>

#include "engine/common/Color.hpp"

namespace N2Engine::Text
{
    /**
     * Outline, shadow and edge softness for SDF text (Rendering::TextRenderer and UI::UIText). Every length
     * is in ems (multiples of the font size), so an effect keeps its proportions at any size. The defaults
     * are all off, and text without effects draws exactly as it did before effects existed.
     *
     * The effects are uniforms of the text material, set on each draw (like the colour): changing them
     * never lays the text out again or creates a material or texture.
     *
     * Limits: the glyph quads only cover the font's SDF spread (AtlasSettings::spreadPx / basePx ems
     * around each glyph, 1/6 em for the default font), so at draw time the effects are reduced to fit in
     * 90% of it: outline + softness / 2, and |shadow offset x| (and y) + outline + shadowSoftness / 2. One
     * warning per process says when that happens. The settings themselves are kept as set. A font with a
     * larger spreadPx (in its .meta) allows wider effects. See docs/text.html#effects.
     *
     * The software renderer can't blend: it alpha-tests face, outline and shadow at their edges and
     * ignores both softness settings.
     */
    struct TextEffects
    {
        /// Outline width in ems, outward from the glyph edge; 0 (the default) draws no outline
        float outlineWidth = 0.0f;
        /// The outline's colour (drawn under the face, which covers it where they overlap)
        Common::Color outlineColor{0.0f, 0.0f, 0.0f, 1.0f};

        /// The shadow's offset in ems: +x right, +y up (down-right is (0.05, -0.05))
        Math::Vector2 shadowOffset{0.0f, 0.0f};
        /// The shadow's colour. The shadow is drawn only when its alpha is above 0, so the default
        /// (transparent) draws none. A shadow at offset (0, 0) with some softness is a glow.
        Common::Color shadowColor{0.0f, 0.0f, 0.0f, 0.0f};
        /// How far the shadow's edge fades, in ems (0 = crisp)
        float shadowSoftness = 0.0f;

        /// How far the text's outer edges fade, in ems (0 = crisp, antialiased over one screen pixel)
        float softness = 0.0f;

        [[nodiscard]] bool HasOutline() const { return outlineWidth > 0.0f; }
        [[nodiscard]] bool HasShadow() const { return shadowColor.a > 0.0f; }
    };
}
