# User shaders, stage 8 and stage 9 run, 2026-11-20

Issue #397, proposal `orchestrator/proposals/user-shaders.md`. One session, run from the goal "merge stage 8, do stage 9, bring the orchestrator files up to date". Every stage was its own branch, worktree (under `.claude/worktrees/`) and PR. Stages 0-7 were already merged when this run started (master `cc972e1b`, then `b4afb3db`).

## What the user decided during the run

- **Stage 8 (#408), after master gained a docs-only merge (#401):** the user said that if the docs-only merge and the earlier CI had passed, the CI re-run could be skipped. Master was merged into the branch, pushed, and #408 merged without waiting for the new run.
- **Stage 9 scope (AskUserQuestion):** all seven candidates, one PR each, in the order 9a blend, 9b isnan, 9c mat3/mat4, 9d vertex stage, 9e 3D/cube, 9f authored WGSL, 9g per-instance overrides. Post-processing excluded. 9f and 9g were built before 9d and 9e (9d and 9e are far larger); the section numbers keep the letters (21 to 27).
- **9d and 9e (a second AskUserQuestion, after 9a-9c, 9f, 9g had merged):** "Do 9d, then 9e."
- **CI rule (end of run):** orchestrator notes should not cost a full CI run, so the docs-only classifier was changed (#418) and merged without waiting for CI, at the user's word.

## Outcome

| Stage | PR | Merge commit | Content | wasm.brotli (size profile) |
| --- | --- | --- | --- | --- |
| 8 | #408 | ece26f5d | Sprite2D, UI Image, ParticleSystem shaders; author docs page | 2,132,660 (stage 7: 2,129,937) |
| 9a | #409 | 31e76f00 | premultiplied and multiply blend | 2,133,483 |
| 9b | #410 | 3831a8b1 | `isnan`/`isinf` through bit-test shims | 2,133,483 |
| 9c | #411 | adf09d2b | `mat3`/`mat4` parameters | 2,135,286 |
| 9f, 9g | #412, #413 | 97eb7a21 (#413 carried #412) | authored WGSL; per-instance shader values in scenes and prefabs | 2,137,980 |
| orchestrator | #414 | d902bbda | files brought current after 9a-9c, 9f, 9g | |
| 9d, 9e | #415, #416 | 64a7de88 (#416 carried #415) | custom vertex stage; cube and 3D textures as atlases | 2,137,905 |
| orchestrator | #417 | 50ac2abe | files brought current after 9d, 9e | |
| CI | #418 | 89562e5e | a Markdown file under `orchestrator/` is a docs-only change | |

No `web/budgets.json` value was touched (size profile limit 2,420,000; the web package profile 2,730,000). Every merge had green CI on the head that merged, except #408 (see above) and #418 (merged without waiting, by the user's word). Proposal sections as built: 21 (9a) to 27 (9g); the execution notes and `CONTINUE.md` carry the state.

## Decisions made without the user, and why

- **9e as atlases, not hardware samplers.** A real cube or 3D texture needs another descriptor type in the pipeline layout every material shares (or per-shader layouts), an `ITexture` creation API on four backends, GL texture units and an asset format for the sources: the largest change of the project, touching every draw, verifiable only on GPUs. The two kinds became parameter types over the existing 2D slots (a strip of six faces, or `depth` slices side by side) with lookup functions in the cook's prelude. Nothing in a backend, a layout or the blob changed. What it cannot do is in proposal section 25 item 6.
- **9d as a hook, not a user `main`.** The author writes `void n2_vertex()`; the engine's generated `main` runs the default vertex maths, the hook, then writes the clip position unless `n2_SetPosition` was called. The interface stays at five varyings, so there is no varying matching. To keep the Vulkan validation layer quiet (an unread vertex output is a warning, and the Vulkan tests fail on warnings) the SPIR-V targets' fragment stage is wrapped so it reads all five.
- **9d and 9e shaders are cooked-only:** the C++ twin of the cook (runtime compile) refuses them with a message instead of duplicating the new templates.
- **9g design:** the shader and its values are saved as `shaderState` on Sprite2D, Image and ParticleSystem, written only when a shader is set (every existing file unchanged), as an unlisted optional member (`RegisterOptionalState`), so it is in no schema, no replication and no snapshot. Prefab instances keep their own values because an instance is a copy of the prefab JSON; the EN4 overrides indicator and an inspector widget are not done (the editor frontend is outside the repository).
- **Stacked PRs.** 9b, 9c, 9f, 9g and 9e each branched from the one before, so the as-built section file never conflicted; the top PR of a stack merged the others (#412 with #413, #415 with #416, and #410 and #411 in order).

## CI failures found and fixed before merging

- 9a: a test wrote its shader and `.mat` files after the project had been indexed (the fixture indexes in `SetUp`).
- 9b: the corpus test counted expectations, not files, when the isnan negative became a positive (nine, eight, then seven).
- 9c: an unset `mat3` defaulted to the first nine floats of a 4x4 identity (a real bug); a test used `mat4` as its unknown type; a `.mat` round-trip test compared two different shader instances.
- 9d: the web build refuses a runtime compile before it mentions the vertex stage, so the test of the message accepts that wording.
- 9e: the test fixture loader did not know the new parameter types (a Vulkan failure on arm64 only showed it first).
- Flakes: the iOS-simulator "first frame" step failed once on #416 and passed on a rerun of the failed job.

## What was verified how

Local: the node suites (the one test that fails on Windows is the CRLF line-ending test, which passes on Linux), the real glslang and Tint on every new corpus file and fixture, and g++ syntax checks of every changed C++ file; the 9d blob tests were compiled and run locally (18 of 18). Everything else ran in CI: Vulkan on llvmpipe with validation on, WebGPU on Dawn with lavapipe, the web job, the wgsl job with Tint and naga. Not run anywhere: OpenGL (no GL in CI), a real GPU, a browser.

## Still the user's

The GL checklist (proposal section 16) with a premultiplied and a multiply draw, a `mat3`/`mat4` parameter with a non-trivial matrix, an `isnan` shader, a custom vertex stage moving geometry (the OpenGL vertex text was never compiled by a driver) and cube and 3D atlas lookups; an authored-WGSL shader and a vertex-stage shader in a browser; in the editor, saving a `.n2shader` and reading `GetShaderInfo` (stage 6); the stage 7 runtime-compile and shader-cache check; Sprite2D, Image and ParticleSystem with a shader on each backend (stage 8); a scene with a shaded Sprite2D saved and reloaded (9g).

## Follow-ups, none started

A vertex stage and the atlas lookups in the runtime compile; hardware cube and 3D samplers; an inspector widget for `shaderState` (needs the editor frontend and a protocol change); the EN4 prefab overrides indicator and revert; textures and the lighting block in the vertex stage.
