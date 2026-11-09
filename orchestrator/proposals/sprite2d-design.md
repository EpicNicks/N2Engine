# Sprite2D and shape rendering: design

Item: `orchestrator/items/sprite2d-shapes.json` (gated: this document first, code only on the user's go).
Written 2026-11-09 against `master` at `ff21edf4`. Phase E2 of the Triangle-Like plan
(`E:/Code/Projects/Triangle-Like N2Engine/PLAN.md`): "Add `Sprite2D` (and procedural shape meshes: triangle, circle, rect,
outline): color, optional texture, sorting order, flip, Blend alpha. Most of the game art is basic geometry, so shapes first,
PNGs where needed."

**Status: approved by the user on 2026-11-09, with all five recommendations in section 4 (A to E) accepted.**

## 1. Summary

One new component, `Sprite2D`, that draws a flat shape in the object's x, y plane: a rectangle, triangle, circle, regular
polygon or custom convex polygon, filled or as an outline, in a colour, optionally textured (a whole image or one cell of a sprite
sheet), with flip, pivot and an integer sorting order. It draws in the existing Transparent queue with the existing unlit material
path, so it needs no renderer changes, and it works on every renderer the engine has. Lua bindings, serialization and docs are
in the same item.

## 2. What the engine already has

Checked in the code, not assumed:

| Need | Already there | Gap |
| --- | --- | --- |
| A flat quad | `QuadRenderer` (built-in 1 x 1 quad on x, y, facing +Z) with colour, size, material | No other shapes, no texture region, no flip, no sorting order |
| Draw order | Transparent queue sorted by `RenderQueueKey::sortKey`, then back to front by depth, stable in hierarchy order (`Scene.cpp`) | Only reachable through a material's alpha mode; no component sets `sortKey` |
| Blending | `Material` alpha mode `Blend`: Transparent queue, `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, depth tested, no depth write | **The software renderer cannot blend**: Blend draws opaque there |
| Textures | `Texture` with per-texture `filter` (`nearest` for pixel art) in its settings; `Material::baseColorTexture`; UI `Image` already draws sprites | No texture region (atlas cell) |
| Arbitrary meshes | `Mesh::Create(MeshData)`: vertices with position, uv and colour | No shape generator |
| 2D camera | Orthographic projection on `Camera` | None |

## 3. Proposed design

### The component

`Sprite2D` (an `IRenderable`, registered, serializable, Lua-bound). Fields, all serialized:

| Field | Default | Meaning |
| --- | --- | --- |
| `shape` | `Rect` | `Rect`, `Triangle`, `Circle`, `RegularPolygon`, `Polygon` |
| `size` | (1, 1) | World units. For `Circle` and `RegularPolygon` the diameter on each axis, so an ellipse is possible |
| `sides` | 6 | `RegularPolygon` only, 3 to 64 |
| `vertices` | none | `Polygon` only: 3 to 64 points, convex, in units of `size` |
| `outline` | 0 | 0 fills the shape. Above 0, only a border of that width in world units, kept the same width at every side |
| `color` | white | Multiplies the texture |
| `texture` | none | An image asset |
| `region` | (0, 0, 1, 1) | The part of the texture used, in uv: one sprite of a sheet |
| `flipX`, `flipY` | false | Mirror the shape and its texture |
| `pivot` | (0.5, 0.5) | Where the object's position sits within `size` |
| `sortingOrder` | 0 | Lower draws first (further back) |
| `material` | none | Optional, for later effects; none uses the engine's unlit material |

The shape lies in the object's local x, y plane, so it rotates and scales with the transform, like everything else.

### Meshes

Generated on the CPU by a pure function, `MakeShapeMesh(shape description)`, and cached by that description. A thousand bullets of
one shape share one mesh. Circles use enough segments that the edge error stays under 1% of the radius, capped at 64.
`outline` builds a ring with mitred corners, so the border width is exact. UVs map the shape's bounding box onto `region`, so a
textured circle shows the middle of its image.

### Drawing

- **Always the Transparent queue,** with `sortKey = sortingOrder`. That is the only queue with a defined order, and it is what makes
  `sortingOrder` work. Equal orders draw back to front by z, then in hierarchy order, which is deterministic.
- **Cull none,** so `flipX`, `flipY` and a negative scale never make a sprite vanish.
- **Depth tested, not written,** the Transparent state: 3D geometry in front of a sprite still hides it.
- **On the software renderer,** which cannot blend, a sprite with a texture draws as a cut-out at alpha 0.5, so transparent pixels
  are not drawn as black boxes. Partial transparency is lost there; solid shapes look identical.

### Lua

`AddComponent("Sprite2D")` with setters and getters for every field, `Sprite2DShape.Rect` and the others, and
`SetTexture("res://...")`. Same checked handles as every other component.

### Not in this item

Glow, outline glow, rainbow, glitch, grayscale and time-stop effects (the plan's "Effects", item 3: materials or a full-screen
pass, a later item); sprite animation (a script can change `region`); batching many sprites into one draw; nine-slice; sorting
layers by name; editor gizmos.

## 4. Decisions needed

| | Decision | Recommendation |
| --- | --- | --- |
| A | One component with a `shape` field, or a `SpriteRenderer` plus separate shape renderers | **One component.** The game's art is mostly coloured shapes with an occasional texture, and one type keeps scenes, Lua and the converter from Unity simple |
| B | Always the Transparent queue, even when opaque | **Yes.** `sortingOrder` only works there. The cost is no early depth rejection for 2D, which does not matter at this scale |
| C | Sorting: an integer `sortingOrder`, or named sorting layers as in Unity | **Integer only for now.** Layers can map onto ranges of it later without changing scenes |
| D | The software renderer's missing blending: cut-out for textured sprites | **Yes, cut-out at 0.5.** The alternative is adding blending to the software renderer, which is a renderer project of its own |
| E | Name | **`Sprite2D`**, as the plan says, matching `Rigidbody2D` |

## 5. Risks

| Risk | Handling |
| --- | --- |
| Many sprites: one draw call each | Measured in the PR with a 2,000-sprite scene on the renderers CI runs; batching is a follow-up if it is slow |
| Web size budgets: the size profile has 1.7% raw headroom after #314 | Measured; a raise, if needed, is asked for with the numbers, as for #314 |
| Mesh generation edge cases (degenerate polygons, outline wider than the shape) | Unit tests; bad input draws nothing and warns once, like the 2D colliders |

## 6. Tests

Shape mesh generation (vertex counts, winding, bounds, uv region, flip, outline width, bad input); sorting by `sortingOrder` and
then depth through `Scene` render collection; serialization round trip; Lua; and pixel tests on the software renderer, which CI
runs headless: a red circle over a blue rect by order, a flipped textured sprite, and a cut-out texture.

## 7. Stages

One PR: the component, the mesh generator, Lua, docs (`docs/sprite2d.html`) and tests. It touches no renderer code. If the
measurement in section 5 shows a problem, batching becomes its own item.
