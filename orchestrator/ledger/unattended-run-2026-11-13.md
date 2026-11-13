# Unattended run, 2026-11-13

Three engine parts, each on its own branch, worktree and PR. Nobody watched this run; every point where a person would
normally have been asked is written here with the choice made. Scope: engine only.

Start state: master 1635a32f, latest master Tests run 43367816378 green. Open PR not created by this run: #357
(feat-web-gamepads), left alone.

Worktrees made by this run (all from origin/master 1635a32f, under .claude/worktrees/):

| Part | Worktree | Branch |
| --- | --- | --- |
| 1 Effects E1 | effects-e1 | feat-effects-e1 |
| 2 UIText per-character colour and scale | uitext-char-style | feat-uitext-char-colour-scale |
| 3 Web CI job time | web-ci-time | ci-web-job-time |
| Docs (this ledger, CONTINUE.md, progress log) | docs-unattended | docs-unattended-run-2026-11-13 |

## Outcome

| Part | PR | Branch | Head | CI | Review |
| --- | --- | --- | --- | --- | --- |
| 1 Effects E1 follow-up | #358 | feat-effects-e1 | 3b29aa04 | all green, mergeable | high: 6 found, 4 fixed, 2 left open |
| 2 UIText per-character colour and scale | #359 | feat-uitext-char-colour-scale | 1a49ed50 | all green, mergeable | high: 9 found, all fixed (rgb face-only by design) |
| 3 Web job ccache | #360 | ci-web-job-time | 746e9f4f | all green, mergeable | high: 7 found, all fixed (in 9071bbf7; 746e9f4f, a one-line fix of that commit's mistake, was not re-reviewed) |

Part 3 measurements (web job; before = master without ccache):

| Run | Job | Build (dev) | size pkg | size w/o Box2D | no physics + no particles | speed pkg |
| --- | --- | --- | --- | --- | --- | --- |
| master 43367816378 | 66m52s | 1059 s | 545 s | 513 s | 912 s | 682 s |
| master 43361334672 | 83m08s | 1347 s | 684 s | 647 s | 1141 s | 843 s |
| master 43356373164 | 87m02s | 1368 s | 726 s | 693 s | 1221 s | 883 s |
| #360 43379645421 (install failed, no cache) | 84m27s | 1364 s | 685 s | 657 s | 1181 s | 848 s |
| #360 43390550976 (ccache) | 52m11s | 1097 s | 594 s | 138 s | 277 s | 706 s |

ccache in 43390550976: 1631 cacheable calls, 1128 hits (69.2%), 503 misses, 43 uncacheable, 0.2 GB; install 9 s.

## Unsure points (options, choice, why)

- **Part 1: E1 was already merged.** CONTINUE.md (2026-11-12) still listed E1 as next, but `saturation`, `flash` and
  `Graphics.CanBlend()` merged in #327 on 2026-11-11 (four backends, Lua, docs, tests). Options: (a) no PR for part 1;
  (b) redo E1 (impossible without duplicating #327); (c) a small PR for what #327 left against the brief's rule "a feature
  that needs blending must degrade or warn on the software renderer". Checked the code: `Sprite2D` cuts a texture out at
  half alpha where `SupportsBlending()` is false, but the UI `Image` did not, so a textured `Image`'s transparent texels drew
  as opaque boxes there. Chose (c): the same cut-out in `Image::RenderUI` (only the software path changes; GPU cutoff stays
  0). Smaller and reversible; PR #358 (branch feat-effects-e1).
- Part 1: no warning log was added for blended draws on the software renderer (degrading by cut-out is what Sprite2D
  does; a per-draw warning would be noise). Particles' additive mode on software was not looked at beyond noting it exists
  (out of E1's scope).
- Part 1: no new Lua API, so no new Lua test; `Graphics.CanBlend()`'s Lua test came with #327.
- **Part 2: cleared on text change?** The brief said per-character values are "cleared on text change", but #336's offsets
  are deliberately KEPT across `SetText` (its code, header comment and docs/ui.html). The brief also says "mirror #336's
  lifecycle". Options: (a) keep across SetText like the offsets; (b) clear colour and scale on SetText (inconsistent with
  offsets); (c) change offsets too (behaviour change to merged API). Chose (a): consistent, no change to existing API.
  Stated as a deviation in PR #359.
- Part 2: per-character colour reaches the face only (vertex colour); outline, shadow and extra passes keep their colours,
  so a character at alpha 0 still shows its outline/shadow. Options: shader change on four backends to multiply passes by
  vertex colour, or document it. Chose to document (no shader change; smaller). Scale is about the glyph quad centre.
- Part 2: values sanitised (colour components clamped 0..1, non-finite = 1; scale negative = 0, non-finite = 1). #336's
  offsets are not sanitised; left as they are.
- **Part 3: ccache vs a parallel job.** Chose ccache via `EM_COMPILER_WRAPPER=ccache` (verified in the local emsdk 4.0.10
  source: `config.COMPILER_WRAPPER` is put in front of clang in `emcc.py` `phase_compile_inputs`; Emscripten's own test
  `test_compiler_wrapper_ccache`). Reason: keeps the `web` check's steps in one job unchanged; a parallel job would change
  what `web` verifies. **Within the run only, no save/restore**: the repo's Actions caches were 10.6 GB of 10 GB on
  2026-11-13 (Windows ccaches about 1.1 GB each, three generations), so a saved web cache would evict the other legs'.
  Cross-run save is held for the user. PR #360.
- Part 3: the brief said the job takes about 58 minutes; master runs 43367816378, 43361334672, 43356373164 took 66m52s,
  83m08s, 87m02s. The 87-minute run is close to the 90-minute timeout (not changed).

## Deviations from design docs

- Part 1: E3 (outline texture) skipped, as the brief and the design allow (optional; nothing shows the recipes fall short).
- Part 1: the PR is a follow-up to #327, not E1 itself (already merged).
- Part 2: kept across SetText instead of "cleared on text change" (see unsure points).
- Part 3: docs corrected in passing on the lines touched: testing.html web job timeout 60 -> 90 minutes (since #323);
  web-build.html "uploads nothing" removed (the job uploads artifacts).

## Checks not run

- Nothing builds locally for the engine: CI is the build for all three PRs.
- Part 1: no browser run of the web player's software fallback with a textured UI Image; no GPU run (the GPU path is
  unchanged: cutoff 0).
- Part 2: no GPU pixel test (OpenGL, Vulkan, WebGPU) of a coloured or scaled character; the text shaders are unchanged and
  already multiply by the vertex colour. Software pixel tests only. No browser or live editor check.
- Part 3: nothing beyond CI itself.

## Blockers

- None that stopped a part. Pushing and `gh pr create` worked; no local mode.
- Part 3 setback (own mistake, fixed): review fix 9071bbf7 set `CCACHE_COMPRESS: "0"`, which ccache rejects ("invalid
  boolean environment variable value"). The `Install ccache` step failed, `continue-on-error` kept the job going without
  ccache (the designed fallback), so run 43379645421 measured no cache (web job 84m27s; size without Box2D 657 s, no
  physics + no particles 1181 s). Fixed in 746e9f4f (`CCACHE_NOCOMPRESS: "true"`); the measurement is from that head.
- `gh` run from the session's start folder resolves another repository: every `gh pr` call needs `-R EpicNicks/N2Engine`
  (one `gh pr edit` failed for that and was rerun with it).

## Reviews (code-review skill, high, one per PR)

- **#358** (6 findings). Fixed (3b29aa04): colour alpha 0 gave cutoff 0, so a fully faded textured Image or Sprite2D drew
  as a box on software (both had it); the rule is one helper, `Renderer::Common::BlendCutOutBelow` (floor half an 8-bit
  step); tests for the raw SetTexture path, alpha 0 (Image and Sprite2D) and the rule; the CanBlend badge placement and
  link.
- **#359** (9 findings). Fixed (1a49ed50): per-character alpha now fades outline, shadow and passes (GL, Vulkan, WGSL
  multiply their alpha by fragColor.a; software tests them against the vertex alpha; the parity test's pinned lines
  updated); software fade documented as solid-then-cut; an index above `UIText::MaxCharacterIndex` (65535) ignored for all
  three lists, offsets included; trailing defaults dropped (offsets included); one pass per rebuild, colours rebuilt only
  when needed; the copied layout released; `Color::White`; the testing.html row's columns.
- **#360** (7 findings). Fixed (9071bbf7): `CCACHE_DISABLE` on the dev and speed steps; `timeout-minutes: 3` on the
  install; statistics on `always()`; the comment no longer claims the Linux legs' settings (compression off, mtime check,
  stated why); docs and comments no longer claim byte-identical output (`CCACHE_BASEDIR` makes `__FILE__`/debug paths
  relative); stale "ccache is not used" and job-time sentences corrected.

## Review findings left unfixed

- #358: a non-finite colour alpha still draws a box on the software renderer (the shader multiplies by it; the cutoff
  cannot fix NaN). Sanitising UIGraphic colours is outside the part.
- #358: doing the cut-out inside the software renderer for every blended textured draw (would cover blended Models too)
  is a wider behaviour change, its own item.
- #359: per-character rgb tints the face only (outline and shadow keep their colours; only alpha follows). By design.
- #359: offsets' semantics changed slightly by the shared rule (index cap, trailing zero offsets dropped from
  GetCharacterOffsets). Not a finding, a consequence of fixing one; stated in the PR body and docs.
