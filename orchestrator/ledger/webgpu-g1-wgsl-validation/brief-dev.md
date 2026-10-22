# Task webgpu-g1-wgsl-validation: WebGPU G1: hand-written WGSL for the unlit, lit and text shaders, a parity test against the shared shader interface table, and WGSL validation by Tint and naga in CI
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-webgpu-g1-wgsl.

## Issue / plan
USER DECISIONS (2026-10-20, final, do NOT revisit): the design in C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md is accepted with ALL recommendations W1-W10: hand-written WGSL for the three built-in shaders tied to the shared interface table by a parity test and validated by Tint (and naga) in CI (W1); WebGPU goldens run natively on Dawn over lavapipe in a new NON-REQUIRED Linux CI job (W2, G2 and later); one wasm containing both renderers chosen at load (W3); WebGPU is the automatic default when the browser offers it with a ?renderer= override (W4); device loss = one silent re-acquire then a 'continue with software' button (W5); emdawnwebgpu with the device acquired in loader JS before callMain, no ASYNCIFY (W6); devicePixelRatio with a 2560 cap (W7); the native WebGPU backend exists only for the CI harness, option OFF by default, not selectable in lua_project or the editor host (W8); v1 = built-in shaders only, wireframe ignored with one warning, no compute, particles/soft bodies on CPU paths (W9); G0-G2 now, G4 after the stage 5 PR #184 merges (W10). Read C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md FULLY (sections 3-7 and the staged plan) before starting. THIS ITEM IS PR G1 and REQUIRES G0 (webgpu-g0-shared-gpu-pieces) TO BE MERGED FIRST (the shared GpuShaderInterface table); do not start the developer until G0 is in master. SCOPE per the design (sections on shader strategy and the G1 row): (1) renderer/shaders/webgpu/{unlit,lit,text}.wgsl hand-written ports of the three built-in shaders (read the Vulkan GLSL shaders and VulkanShaderInterface/GpuShaderInterface to mirror the interface exactly): handle the issues the design lists: row_major matrix blocks (WGSL cannot express them: use the transpose-on-upload convention and document it), lit.vert's inverse() (write an inverse helper in WGSL), combined image samplers (separate texture and sampler bindings), derivatives/uniformity for text and lit (fwidth, textureSample after discard: keep it valid under WGSL uniformity analysis, the CI validator is the proof); (2) a WebGpuShaderParityTests.cpp (a pure text/parse test, no GPU, runs on every leg that runs renderer tests): parses the .wgsl files (bind groups, bindings, struct member offsets/sizes/types) and compares to the shared interface table, and checks every uniform name Material::ApplyUniforms sets exists in the corresponding WGSL; deterministic failure messages naming the file/binding; (3) WGSL VALIDATION IN CI: pick the lightest robust way to run Tint (and naga) on the three files in an existing Linux job or a new small NON-REQUIRED job (state the added minutes): pinned versions with hashes (e.g. prebuilt tint/naga release binaries, or cargo-installed naga-cli with a lockfile/pin, or the Dawn tint target if G2's Dawn is not available yet: justify and pin), the job fails on any validation error or warning-as-error you choose and is documented; do NOT add a required check and do NOT rename existing jobs; docs-only skip pattern must be followed for any new job; (4) embed the WGSL as strings through CMake for later use by the backend (a generated header or a resource library target, native and web safe; nothing links WebGPU yet and N2ENGINE_RENDERER_WEBGPU does not exist yet: do NOT add it); (5) DOCS: a section in the rendering docs (and docs/testing.html) describing the WGSL files, the conventions (transpose-on-upload, separate samplers, inverse helper), the parity test and the validation job, and THIRD_PARTY_NOTICES for Tint/naga if you ship or fetch them in CI only (state which); verify against the code. Nothing builds locally; CI is the test; expect fix rounds. Unverified: actual rendering (G2/G3).

## Allowed paths (regex, repo-relative)
- ^renderer/
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^scripts/
- ^THIRD_PARTY_NOTICES
- ^\.github/workflows/tests\.yml$

## Acceptance
- Three WGSL shaders exist mirroring the shared interface table; conventions documented
- Parity test (no GPU) compares WGSL bindings/offsets/uniform names to the table and Material::ApplyUniforms
- Tint and naga validate the shaders in CI (non-required job or existing job, pinned, minutes stated)
- WGSL embedded for later use; no WebGPU backend/option yet; all 10 checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: WGSL semantics (alignment/std140-vs-WGSL layout rules, matrix transpose convention applied consistently, uniformity of derivatives and sampling, inverse helper correctness, sampler/texture split, depth range and clip-space notes), parity test is meaningful and fails on a mismatch, validator pins and hashes real, CI job not required and follows the docs-only skip, no new required check, docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs webgpu-g1-wgsl-validation` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-webgpu-g1-wgsl` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g1-wgsl-validation/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-webgpu-g1-wgsl --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g1-wgsl-validation/dev.json: {"pr":<number>,"branch":"feat-webgpu-g1-wgsl","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-webgpu-g1-wgsl` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
