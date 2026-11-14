# Effects for 2D sprites: design

Item: `orchestrator/items/effects.json` (gated: this document first, code only on the user's go).
Written 2026-11-09 against `master` at `66a49038`. It builds on `Sprite2D` (PR #318, branch `feat-sprite2d-shapes`, not merged
yet). It was first written for a 2D game port's six effect shaders (glow, outline, rainbow, glitch, grayscale, time-stop),
which section 1 reads as examples. Since 2026-11-12 the remaining stages (E1, E3) are engine features judged on their own;
the game tracks its own needs in its own folder (it did its effects without them).

**Status: approved by the user on 2026-11-09, with every recommendation in section 5 (A to H) accepted.** E2 starts first, from master: saturation only, since whether time-stop also inverts colours (section 2) still has to be checked in Unity by eye. E1 starts after #318 merges.

## 1. Summary

The game has six effect shaders. Read in the Unity project, most are simpler than they sound:

- **Rainbow** is one colour per sprite that cycles over time. **Grayscale** is a per-sprite shader on the item gallery icons
  (UI images), not a full-screen effect.
- **Outline** and **glow** can be drawn with extra sprites. No shader is needed for shapes.
- **Glitch** is used once (the Triclone turret).
- **Time-stop** is the only real full-screen effect: a circle that grows from the screen centre and desaturates what is inside.

The engine has no custom shaders on three of its four renderers and no full-screen pass. This design does not add either.
It proposes two small engine changes, each on all four renderers:

1. **Sprite colour effects:** a `saturation` and a `flash` colour (with an amount) on `Sprite2D` and the UI `Image`, added to
   the built-in unlit shader. This covers grayscale, rainbow and a white hit flash.
2. **Screen tint:** a frame-wide colour change that every scene draw applies to itself, inside or outside a circle on the
   screen. It works the way the editor's prefab-stage dim already works. This covers time-stop and a full-screen grayscale,
   with no render target and no extra pass.

Outline, glow and glitch become recipes in Lua with existing engine parts, plus one optional helper for outlines of textured
sprites. Animation (time) is driven from Lua each frame; the shaders get no clock.

## 2. What the six Unity effects do

Read in `C:\Users\epicn\Unity Projects\Triangle-Like\Assets\Shaders` and the scripts that use them:

| Effect | Unity source | What it does | Where the game uses it |
| --- | --- | --- | --- |
| Outline | `OutlineColorShader.shader` | Samples the texture's alpha at 8 directions per pixel of width and draws `_OutlineColor` where a neighbour is solid and this pixel is not | Homing projectiles (`Projectile.cs` swaps in `HomingProjectileSpriteOutlineColor.mat`) |
| Moving outline | `BorderMovingOutline.shadergraph` | Pushes vertices out along their normals by `sin(time) * WaveMagnitude * OutlineWidth`, in `OutlineColor` | Not traced to a prefab in a quick search |
| Glow | `SimpleGlow.shadergraph` | No texture: alpha is `pow(1 - length(position), GlowFalloff) * GlowIntensity` on a quad, times the vertex colour. A soft disc | Not traced to a prefab in a quick search |
| Rainbow | `RainbowFlashSprite.shadergraph` | Samples a gradient at `frac(time * RainbowSpeed)` and mixes it over the sprite by `RainbowIntensity`. One colour for the whole sprite at a time | `ItemPedestal.cs` has a `RAINBOW` mode; text rainbow is separate (`TextEffectManager.cs`, per character, CPU) |
| Grayscale | `GrayscaleShader.shadergraph` (a sprite shader) | `lerp(colour, dot(colour, luma), GrayScaleAmount)` | `ItemGalleryIconButton.cs`: an uncollected item's icon is grey (amount 0 or 1) |
| Glitch | `SpriteGlitchShader.shader` | Time-stepped block noise moves the uv, red and blue are sampled with a sideways offset (chromatic aberration), random thin bars shift | `TricloneActiveItem.cs`: the turret |
| Time-stop | `Time Stop Full Screen Effect.shadergraph` (URP full-screen pass) | Reads the frame, Saturation node at 0.2 inside a circle around the screen centre (aspect corrected) of `_Radius` | `FullScreenEffectManager.cs`: radius 0 to 2.5 in 0.5 s, held, then back to 0 in 0.25 s |

A seventh shader, `ScrollingImageShader.shader` (dialogue background, scrolling tiles), only moves the uv over time. With
`Sprite2D`'s `region` and a texture whose wrap is `repeat`, Lua can do it with no engine change.

**Not verified:** the time-stop graph has two Negate nodes and a Step against -0.3. I could not tell from the file alone whether
the circle edge is hard or whether part of the image is also inverted. Someone should look at it running in Unity before stage
E2 picks the exact maths.

## 3. What the engine already has

Checked in the code on `master` (and PR #318 for `Sprite2D`), not assumed:

| Need | Already there | Gap |
| --- | --- | --- |
| Custom shaders | `IRenderer::CreateShaderProgram(vertex, fragment)`. Works on **OpenGL only**. Vulkan returns null (`VulkanDraw.cpp`: "custom GLSL shaders are not supported"), WebGPU returns null (`WebGpuDraw.cpp`), software ignores the source and returns its unlit shader (`SoftwareRenderer.cpp`) | No custom shader path on Vulkan, WebGPU or software. `Material` has no shader field: `ShadingModel` picks one of the built-in shaders |
| Built-in shaders | Unlit, lit and text, in four forms: OpenGL GLSL 330 strings in `OpenGLRenderer.cpp`, Vulkan GLSL in `renderer/shaders/vulkan/` (SPIR-V at build time), WGSL in `renderer/shaders/webgpu/`, and C++ in `SoftwareRenderer.cpp` | No effect uniforms on the unlit shader beyond colour, texture and alpha cutoff |
| Keeping the four in sync | `GpuShaderInterface.hpp` is the uniform table; `VulkanShaderParityTests.cpp` and `WebGpuShaderParityTests.cpp` check the shaders against it; `CrossBackendDiffTests.cpp` compares software with Vulkan | OpenGL is not run in CI |
| Per-sprite values | `Material::ApplyUniforms` sets uniforms before every draw, so a value that changes every frame costs nothing extra (`Material.hpp`). `Sprite2D` owns one private unlit Blend material (`Sprite2D.cpp`, `EnsureMaterial`). UI `Image` draws with its own material on the standard unlit shader (`ui/Image.cpp`) | No saturation or flash value |
| A per-draw colour change on every backend | `RenderState::dim` and `IRenderer::SetStageDim` (`RenderState.hpp`, `Renderer.hpp`): a frame-scoped grey mix that every draw recorded while it is set carries in its state, applied by all three built-in shaders on all four backends (`SoftwareStageDimTests.cpp`). `Application.cpp` sets it around the scene and resets it before the UI | Fixed grey (0.45), the whole screen only, no circle |
| Full-screen pass | None. No post-processing, no tonemap, no render-to-texture step in the frame. The only full-screen draws are presenters: Vulkan blits its offscreen image to the swapchain, the software renderer uploads its CPU buffer to a GL texture, the web loader draws a full-screen triangle of the WebGPU offscreen texture onto the canvas (`n2-loader.js`) | The scene and the UI are drawn into one target in one frame (`Application::Render`), so a pass between them would need the frame split, a copy of the colour target and a new pipeline on each backend |
| Blending | Normal (`SRC_ALPHA, ONE_MINUS_SRC_ALPHA`) and additive (`RenderState::additive`) on OpenGL, Vulkan and WebGPU | **The software renderer cannot blend.** `Sprite2D` cuts textured sprites out at alpha 0.5 there |
| Shape outlines | `Sprite2D.outline`: a ring of exact width around any shape | None for textured sprites |
| Time in shaders | None (no `uTime` anywhere) | Not needed: Lua drives every value per frame |
| Which renderer players get | Desktop: chosen in the app options. Web: the shell probes WebGPU and falls back to software (`web/player/WebPlayer.cpp`) | The software renderer is a real player path on the web, not only the test renderer |

## 4. Proposed design

### 4.1 Sprite colour effects (stage E1)

Two fields on `Sprite2D` and on the UI `Image`, serialized and Lua-bound:

| Field | Default | Meaning |
| --- | --- | --- |
| `saturation` | 1 | 1 is the colour as drawn; 0 is grey (Rec. 601 luma, the weights `RenderState` already uses); values in between mix |
| `flash` | transparent | A colour mixed over the sprite by its alpha: `rgb = mix(rgb, flash.rgb, flash.a)`. Sprite alpha is unchanged, so the shape stays the same |

Order in the shader: texture times colour, then saturation, then flash, then the stage dim and screen tint (4.2). Each is skipped
when it has its default value.

How each renderer does it:

- **OpenGL, Vulkan, WebGPU:** two uniforms on the unlit shader's material block (`uSaturation`, `uFlash`), added to the table in
  `GpuShaderInterface.hpp`. `Material` gets the two fields and sets them in `ApplyUniforms`. A few lines per shader.
- **Software:** the same maths in C++ in the unlit pixel path, next to `DimPacked`. Identical result per pixel, since the
  software renderer does not blend anyway.

Effects it covers:

- **Grayscale:** `saturation = 0` on the gallery icon, as in Unity.
- **Rainbow:** Lua sets `flash` each frame to a hue that cycles, with alpha = the intensity. For a white shape, setting `color`
  instead gives the same image, so the game can start with `color` before E1 lands.
- **Hit flash:** `flash = white, 0.8` for a few frames. Not one of the six, but a common use of the same field.

### 4.2 Screen tint (stage E2)

A frame-scoped setting on the renderer, made like the stage dim:

- `IRenderer::SetScreenTint(ScreenTint)`, where `ScreenTint` holds a `saturation` (1 = none), a circle `centre` (0 to 1 of the
  screen) and `radius` (in half screen heights), and `inside` (true: the tint applies inside the circle; false: outside).
- Every draw recorded while it is set carries it in its `RenderState`, as `dim` does, so it survives the software renderer's
  reordering.
- Each built-in shader (unlit, lit, text), on each backend, tests its own pixel's screen position against the circle and applies
  the saturation there. On the GPU the pixel position is `gl_FragCoord` or `@builtin(position)`; on the software renderer it is
  the pixel being written.
- `Application::Render` applies it to the scene pass only, and clears it before the UI pass, exactly where the stage dim is reset.
  A Lua API sets it: `Screen.SetTint{saturation = 0.2, radius = r}` and `Screen.ClearTint()` (names to settle in the PR).

Why this gives the same image as a real full-screen pass: a saturation change is a linear map of the colour, and alpha blending
mixes colours linearly, so tinting each draw and then blending gives exactly the image that tinting the finished frame gives.
That holds for normal and additive blending. It would not hold for a colour inversion with additive blending; if the Unity graph
turns out to invert (section 2), inversion is limited to normal blending or left out.

Two limits, both stated in the docs:

- **The clear colour is not drawn by a draw.** It is tinted only when the circle covers the whole screen. A room background drawn
  as a `Sprite2D` is tinted correctly; a plain clear colour inside a partial circle is not. For a dark background this does not
  show.
- **Custom OpenGL shaders** (the only renderer that has them) would not apply it, as they do not apply the dim today. The game
  uses none.

Effects it covers:

- **Time-stop:** Lua animates `radius` from 0 to 2.5 and back, as `FullScreenEffectManager.cs` does, with `saturation = 0.2`.
- **Full-screen grayscale** (death screen, pause): `saturation = 0` with a radius that covers the screen.

### 4.3 Outline (recipes, plus an optional helper in stage E3)

- **Shapes:** a child `Sprite2D` with the same shape, `outline > 0` and a lower `sortingOrder`. Exact on every renderer, no
  blending needed. The moving outline is Lua changing the child's `outline` or scale with a sine.
- **Textured sprites** (the homing projectile, if its art is a texture): a CPU helper,
  `Texture::MakeOutline(source, widthPixels, colour)`, dilates the alpha mask once and returns a new texture, padded by the width,
  cached by its inputs. Drawn as a second sprite behind the first, sized up by the padding. One code path for all renderers;
  on software it is cut out at 0.5 like any textured sprite, which suits a hard outline. The alternative, a dilation loop in the
  shader (8 samples per pixel of width, as Unity does), would be four more shader paths and slow on the software renderer.

### 4.4 Glow (recipe)

A `Sprite2D` with a radial gradient texture (a PNG in the game, or made once in Lua or C++), `Blend`, behind the object. This is
what `SimpleGlow` computes per pixel, baked into an image. Pulsing is Lua changing `color` alpha or scale.

On the software renderer a soft disc cannot be drawn: with the cut-out it becomes a hard disc at half strength. The proposal is
that the game skips glow sprites there. That needs one small binding: `Graphics.CanBlend()` in Lua (the C++ check exists in
`Sprite2D.cpp`). See decision E.

### 4.5 Glitch (recipe)

Lua, every 1 / speed seconds: move the sprite a few pixels at random, shift `region` a little, and set `flash` to red or blue for
one step. For the chromatic look, two tinted copies offset sideways (needs blending; on software they draw opaque, so they are
skipped there like glow). It is used on one object, so a fourfold shader port is not worth it now. If the result looks too
different, a shader mode can follow as its own item.

### 4.6 What each renderer does, in one table

| Effect | OpenGL | Vulkan | WebGPU | Software |
| --- | --- | --- | --- | --- |
| Grayscale, rainbow, flash (E1) | Uniforms | Uniforms | Uniforms | Same maths in C++ |
| Time-stop, full-screen grey (E2) | Per-pixel circle test | Same | Same | Same, on the pixel written |
| Shape outline | Child sprite | Same | Same | Same |
| Texture outline (E3) | Second sprite, CPU-made texture | Same | Same | Same, cut out at 0.5 |
| Glow | Gradient sprite, blended | Same | Same | Skipped (no blending) |
| Glitch | Lua recipe | Same | Same | Without the tinted copies |

### Not in this item

A general custom shader or material graph system; a real post-processing pass (bloom, blur, distortion); a time uniform in
shaders; the glitch effect as a shader; blending on the software renderer (screen-door dithering would be the cheap version; it
is a renderer project of its own).

## 5. Decisions needed

| | Decision | Recommendation |
| --- | --- | --- |
| A | Custom shaders (a shader asset per material, written four times) or a fixed set of effect uniforms in the built-in shaders | **Fixed uniforms.** Three of four renderers have no custom shader path, and each custom shader would need GLSL 330, Vulkan GLSL, WGSL and C++ versions |
| B | Time-stop and full-screen grey: a real full-screen pass (split frame, copy target, new pipeline on four backends) or the per-draw screen tint | **Screen tint.** It is exact for saturation, reuses the stage-dim path, needs no render target, and works on the software renderer the same way. A real pass waits until an effect needs neighbouring pixels (blur, bloom) |
| C | Grayscale: full-screen as the plan says, or per sprite as the Unity game actually uses it | **Both, with no extra cost:** per sprite through `saturation` (E1), full screen through the screen tint (E2) |
| D | Outline of textured sprites: a CPU-made outline texture or a dilation loop in the shader | **CPU texture (E3), and only if the game's outlined sprites are textures.** Shapes use `Sprite2D.outline` today |
| E | Glow and glitch copies on the software renderer, which cannot blend | **Skip them there,** with a `Graphics.CanBlend()` Lua call. Hard-edged discs look worse than no glow |
| F | Glitch: Lua recipe or a shader mode | **Lua recipe.** One object uses it |
| G | Animation: a time uniform in shaders, or Lua sets values each frame | **Lua each frame.** Matches how `FullScreenEffectManager.cs` already drives time-stop, and keeps the shaders stateless |
| H | Does the screen tint reach the UI? | **No, scene only.** In Unity, overlay UI is drawn after URP's full-screen passes, so the HUD stayed in colour (worth a check by eye) |

## 6. Risks

| Risk | Handling |
| --- | --- |
| Four shader languages to keep in step (OpenGL GLSL strings, Vulkan GLSL, WGSL, C++) | Every new uniform goes in `GpuShaderInterface.hpp`; the Vulkan and WebGPU parity tests check the shaders against it; software pixel tests and `CrossBackendDiffTests` compare software with Vulkan (lavapipe in CI). OpenGL is not run in CI: listed as unverified on a GPU in the PR, as for text effects |
| Web size budgets: the size profile had about 1.7% raw headroom after #314, and `Sprite2D` may use some of it | The changes are a few uniforms and lines of shader text and C++, expected well under 10 KB. Measured in each PR; a raise, if needed, is asked for with the numbers |
| Software renderer speed: extra per-pixel maths | Skipped per draw when the values are defaults (one branch per draw, not per pixel). Measured with the 2,000-sprite scene from #318 |
| The software renderer is a player path on the web (WebGPU fallback), so glow is missing there | Stated in the docs; the game decides per effect with `Graphics.CanBlend()` |
| The clear colour is not tinted by a partial circle | Stated; the game draws its room background as a sprite |
| Unity details not verified from files (time-stop's Negate nodes, where glow and the moving outline are used) | Checked by eye in Unity before E2's maths is fixed; the recipes are game code and can change freely |
| Depends on PR #318 | E1 starts after #318 merges |

## 7. Tests

- **E1:** software pixel tests (CI runs them headless): a red sprite at `saturation` 0 is grey of the right luma; `flash` white at
  0.5 is halfway; defaults change nothing (the existing golden images stay identical). Vulkan the same through lavapipe.
  Shader parity tests for the new uniforms. `Sprite2D` and `Image` serialization round trip, old scenes load unchanged. Lua.
- **E2:** software pixel tests: inside and outside the circle, `inside = false`, the circle moved off-centre, a blended sprite over
  an opaque one (tinted per draw equals tinted after); the tint survives the software renderer's draw reordering
  (`DrawOrderTests`); the UI pass is not tinted (`Application` test with a canvas). Parity tests for the new per-draw fields.
- **E3:** the outline texture: padding, width, colour, cache key, a 1 x 1 and a fully transparent source.

## 8. Stages

| Stage | Contents | Touches renderers |
| --- | --- | --- |
| E0 | Nothing in the engine: the game uses `color` for rainbow, child outlines, gradient glow and the Lua glitch now | No |
| E1 | `saturation` and `flash` on `Sprite2D` and `Image`, the unlit shader on four backends, `Graphics.CanBlend()`, Lua, docs, tests | Yes, unlit only |
| E2 | Screen tint: `RenderState` fields, `IRenderer::SetScreenTint`, the three built-in shaders on four backends, `Application`, Lua `Screen`, docs, tests | Yes |
| E3 | Optional: `Texture::MakeOutline`. **Implemented in PR #369** (2026-11-13): `Texture::MakeOutline(source, widthPixels, colour)`, an exact Euclidean dilation (distance transform) of the alpha mask (solid from alpha 128), padded by the width, cached by (source, width, 8-bit colour) up to 32 outlines; Lua `Sprite2D:SetOutlineTextureOf(source, widthPixels, color)` and `Sprite2D:GetTextureSize()`; the recipe with the padding maths in `docs/sprite2d.html#texture-outline` | No |

Each stage is one PR, each with its own web size measurement. E1 and E2 are independent after #318; E2 matters most (time-stop
is the one effect the game cannot fake).
