Vulkan stage E of #43: the SDF text shader draws, antialiased and blended as on OpenGL. Builds on stages 0 (#136), A (#142), B (#144), C (#148) and D (#158). After this only stage F (window, swapchain, present: needs the GPU PC) and the optional G remain, so #43 stays open.

## What changes
- `GetStandardTextShader()` returns the text built-in (nullptr only in a build without the compiled shaders, logged once as for unlit and lit); `IsValidShader` true for it, `DestroyShaderProgram` refuses it.
- `CreateMaterial(textShader[, atlas])` returns a registered material with exactly OpenGL's text defaults (`uAlbedo` white, `uOutline`, `uOutlineColor`, `uSoftness`, `uShadowColor`, `uShadowOffset`, `uShadowSoftness` all 0) written through the std140 name table; `Set*`, `SetColor` and `SetTexture` work as for unlit and lit, a name of another shader logs once through the Warn path. Materials of null or foreign shaders still return nullptr.
- Text draws antialiased and blended per the draw's `RenderState` (stage D's blending): Transparent text writes no depth, UI text no depth test. Outline and shadow are composited under the face in one draw, softness widens the ramp, a fragment under alpha 0.01 is discarded (no colour, no depth).
- Multi-pass effects: one material and mesh, the effect uniforms changed between `DrawMesh` calls, the face last. Each `DrawMesh` snapshots the material block into the ring, so every pass draws with its own values in one frame (no new mechanism; tested at renderer level and through the engine).
- Atlas-less text draws nothing: the text shader's base sampler default is a renderer-owned 1x1 (0,0,0,0) texture (sdf 0, coverage 0, discarded) instead of the white default (sdf 1 would fill the quad). Covers no atlas, `SetTexture(nullptr)` and an atlas destroyed after `SetTexture` (also a new texture at the destroyed one's address). Design callout in application.html (`#design-vulkan-text-atlasless`).
- The four "Vulkan until #43" notes are removed or reworded, comment-only: `TextRenderer.hpp`, `UIText.hpp`, `ParticleSystem.hpp` (stale since stage D), `Image.cpp` (the failed-texture memo stays; the parenthetical now names a size-limit failure). Stale lines in `Renderer.hpp`, `VulkanRenderer.hpp/.cpp`, `VulkanDraw.cpp`, `text.frag` and `VulkanShaderInterface.hpp` updated. `IRenderer` unchanged, no engine behaviour change, OpenGL still the default, the editor host still refuses `--renderer vulkan`.
- Docs: application.html `#vulkan-renderer` now "stages 0, A, B, C, D and E" (PR badges, `#vulkan-text`, the atlas-less callout, remaining: F and optional G), text.html backend table, ui.html, testing.html (rows for every new file, `#vulkan-text-tests`). particles.html checked: its Vulkan lines are still true.

## Tests (renderer_tests, tests/renderer/vulkan/, inside the N2ENGINE_RENDERER_VULKAN guard)
- `VulkanTextShaderTests.cpp` (19 tests, renderer level): the seven `SoftwareTextShaderTest` cases with the same disc atlas, quads and frame and GL expectations (each software assertion that encodes alpha testing or Transparent depth writes has a commented GL one), plus atlas-less and destroyed-atlas, translucent blend, softness, outline, shadow, the same material drawn twice with different values, the TextDrawing pass sequence against the single draw, a mesh updated for 11 frames, pipeline caching in Transparent and Overlay, the exact text defaults and the Warn path, custom GLSL still refused.
- `VulkanTextGoldenTests.cpp` (4 mirrors + 2 cross-diffs), `VulkanTextEffectsGoldenTests.cpp` (6 mirrors + 1 cross-diff), `VulkanTextPassesGoldenTests.cpp` (the 10 rendered cases); helpers in `VulkanTextTestUtil.hpp` and `VulkanTextSceneUtil.hpp`.
- Updated as the brief names: `VulkanPipelineTests`, `VulkanRendererTests.EverythingThatWouldDrawFailsCleanly`. `VulkanShaderParityTests` (device-free) gains three tests: the text defaults against OpenGL's text branch (name, type, value; every table entry has a default), every uniform `TextDrawing.cpp` sets against the table (name and type, and every entry is set), and `text.frag`'s maths from `over` to the end of `main` equal to GL's `textFrag` statement for statement with the named constants and the layer order.
- Software test files are untouched; no earlier assertion weakened.

## Measured cross-diff numbers (lavapipe, Linux Clang x86_64 leg)
Strict 2/255 on the pixels away from the edges (edge border 1, never loosened); every scene had a mean error of 0/255, nothing over the tolerance and a largest difference of 0/255.

| scene | compared | left out (share) | area against software | ink compared |
|---|---|---|---|---|
| SDF disc, 64x64 | 3908 | 188 (4.6%) | 315.5 px against 316 (0.9986) | 240 |
| TextRenderer "Hi", 320x192 | 60176 | 1264 (2.06%) | 1882.7 against 1868 (1.0079) | 1360 |
| UIText "Hi", 400x160 | 62793 | 1207 (1.89%) | 1984.0 against 2005 (0.9895) | 1495 |
| outline + shadow + face, 320x192 | 57799 | 3641 (5.93%) | face + outline 3016 against 3010 (1.002), shadow 1408 against 1407 (1.0007), face alone 0.974, outline alone 1.030 | 1629 |

arm64 leg: the disc is identical, the glyph scenes left out 1248 and 1196 with areas 0.9904 and 0.9690, the three-layer scene left out 3637 with face + outline 0.996 and shadow 0.9993, but its face and outline alone 0.914 and 1.077 (their shared boundary is the one a driver's rounding of a filtered sample moves). Settings in one draw against the same list as passes: largest channel difference 2/255 on both legs.

Caps and floors (documented per scene in the tests): excluded share at most 15% (disc), 25% (glyph scenes), 40% (three layers); at least 50, 300, 500 ink pixels compared; area within 4% (disc) and 5% (glyphs); three layers: face + outline and shadow within 5%, each of face and outline alone within 12%.

## Not verified
- Real GPU drivers: `fwidth` derivative precision and the width of the `smoothstep` ramp on hardware; blending of antialiased edges into a real (sRGB or UNORM) swapchain format; sampling a 1x1 default texture on a real driver. lavapipe only.
- Text at HiDPI framebuffer sizes and anything windowed (stage F).
- Performance of per-frame text mesh updates with synchronous uploads.

## Approved deviations
Accepted by the user 2026-10-13:
- Atlas-less text: chose a renderer-owned 1x1 (0,0,0,0) default for the text shader's base sampler (the brief allowed that or skipping the draw); documented in VulkanDraw.cpp and as a design callout in application.html, tested for no atlas, SetTexture(nullptr) and a destroyed atlas, with and without a depth-writing state.
- Cross-diff tolerances are per scene and documented in the tests: edge border 1 everywhere, excluded-share caps 15% (disc), 25% (glyph scenes), 40% (three layers), a floor of 50/300/500 compared ink pixels, ink-area within 4% (disc) and 5% (glyphs); strict 2/255 on the remaining pixels, never loosened (measured max 0/255 on x86_64 and arm64).
- Three-layer outline+shadow scene: face+outline and the shadow are compared within 5%, but the face and the outline ALONE within 12%, because on the arm64 lavapipe leg their shared boundary classifies 0.914 and 1.077 of the software's (x86_64: 0.974 and 1.030) while their sum agrees to 0.4%.
- Software exact-set equalities become allowances on antialiased frames: classified-to-nearest-layer face sets within 2% (at least 6 px), 'later pass wins' overlaps within 10%, the moved-shadow check 90% instead of the software's 95%, outline-kept checks 3%.
- SoftwareTextEffectsGoldenTest.EffectsOffDrawsExactlyThePlainText: the software 'off' set also sets softness because the software renderer ignores it; on Vulkan softness widens the ramp (GL semantics), so the Vulkan 'off' case leaves it out and a separate check shows softness widening the ramp around the same ink and area.
- SoftwareTextPassesGoldenTest.APassWithoutOffsetOrWidthIsACopyOfTheFace is mirrored as APassWithoutOffsetOrWidthIsHiddenByTheSolidFaceAndOnlyFringesItsAntialiasedEdge: with blending a pass in the face's own shape shows a fringe at the face's edge (at most a quarter of its colour, (1-c)c), where the software test asserts exact equality; the Vulkan test asserts identical pixels where the face is solid or absent and a bounded red-only fringe elsewhere.
- TheShadowAndOutlineSettingsDrawAsTheirPassEquivalents: Vulkan frames are compared to 3/255 (measured 2/255 on both Linux legs) instead of byte-equal, because the single draw blends once and the pass list blends after each pass into the 8-bit target.
- AGlyphsOutlineNeverPunchesHolesInItsNeighboursFace: the software test guards a depth-write problem that Transparent text does not have; the Vulkan test asserts that no pixel deep inside the plain face (all four neighbours face) is not face with the outline on.
- Parity test goes beyond the brief with a statement-for-statement equality of text.frag's maths (from over() to the end of main) with GL's textFrag, besides the named constants and layer order.
- The first commit on this branch (9807bc9, by an earlier attempt of this item) already held the renderer change and the comment-only engine edits; kept as is after re-reading it, and the tests and docs were added on top.
- Measured cross-diff numbers were read from the log of a failing CI run (ctest prints test output only on failure); there is no temporary dump test in the branch.


## Review

An independent review of 5d910bc found no open blocker or high findings; CI is green on all jobs.

