#pragma once

#include <vector>

#include <math/Vector2.hpp>

#include "engine/common/Color.hpp"

namespace N2Engine::Text
{
    /**
     * One extra effect pass of TextEffects::passes: the glyph shape drawn again in one colour, behind the
     * text's face. Lengths are in ems, like the rest of TextEffects.
     *
     * The shape is the face grown outward by `width` (the SDF threshold moved from the glyph edge to
     * `width` ems outside it), moved by `offset`, with its edge fading over `softness`. A pass with only
     * a colour is a copy of the face in that colour; with an offset it is a shadow, with a width an
     * outline, with softness and no offset a glow.
     *
     * The pass is drawn only when its colour's alpha is above 0. Like the other effects it is reduced to
     * fit the font's SDF spread (width + softness / 2 + |offset| <= 90% of it, per axis).
     */
    struct TextPass
    {
        Common::Color color{0.0f, 0.0f, 0.0f, 1.0f};
        /// +x right, +y up
        Math::Vector2 offset{0.0f, 0.0f};
        /// How far outward from the glyph edge the shape reaches; 0 = the glyph's own outline. Not negative.
        float width = 0.0f;
        /// How far the shape's edge fades (0 = crisp)
        float softness = 0.0f;

        [[nodiscard]] bool IsVisible() const { return color.a > 0.0f; }
    };

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
     *
     * Passes: the effects are an ordered list of passes over the glyph mesh, drawn back to front, the face
     * last. The list is: the shadow (if on), the outline (if on), then `passes` in order, then the face.
     * The shadow, outline and softness fields above are the first two entries and the face's softness, so
     * a text that sets only them is drawn in one shader draw, as it always was; `passes` adds more. Each
     * extra pass is one more draw of the same mesh, so keep the list short. See docs/text.html#passes.
     */
    struct TextEffects
    {
        /// Outline width in ems, outward from the glyph edge; 0 (the default) draws no outline
        float outlineWidth = 0.0f;
        /// The outline's colour. The outline fills the whole glyph shape out to its outer edge, under the
        /// face: an opaque face hides the inner part, but a translucent or faded face shows the outline
        /// colour through the whole glyph (a silhouette).
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

        /// Extra passes drawn after the shadow and outline and behind the face, first entry furthest back
        std::vector<TextPass> passes;

        [[nodiscard]] bool HasOutline() const { return outlineWidth > 0.0f; }
        [[nodiscard]] bool HasShadow() const { return shadowColor.a > 0.0f; }
        /// Whether any extra pass would draw
        [[nodiscard]] bool HasPasses() const
        {
            for (const TextPass &pass : passes)
            {
                if (pass.IsVisible())
                {
                    return true;
                }
            }
            return false;
        }
    };
}
