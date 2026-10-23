# Task webgpu-g3b-lit-blend-text: WebGPU G3b: lit materials with aux textures and PBR, the lighting block, blend/additive/Transparent, mid-frame SetViewProjection, UpdateSceneLighting and text with effects on Dawn/lavapipe, goldens against the software renderer and Vulkan
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-webgpu-g3b-lit-blend-text.

## Issue / plan
WebGPU design C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md (W1-W10 ACCEPTED 2026-10-20). G0 (#188), G1 (#190 WGSL lit/unlit/text + parity test + Tint/naga validation), G2 (#196 core, Dawn prebuilt, error counting) and G3a (#201: meshes with the in-frame update rule, textures with CPU mips, pipeline and bind-group caches, uniform ring, unlit draws, goldens vs software) are merged. Read the design sections on the lit shader/material system/lighting block/blend states/text, the G3b row, 'must change for WebGPU' items that apply (row_major transpose-on-upload, separate texture/sampler, uniformity), the merged G2/G3a code (renderer/src/webgpu/*) and the Vulkan backend's lit/blend/text code as the template (renderer/src/vulkan/*, BlockMaterial, GpuLighting, GpuShaderInterface, the text pipeline and glyph atlas path), the WGSL files from G1 and their parity test. THIS ITEM IS G3b ONLY: (1) lit materials: aux textures (normal, metallic/roughness, etc. as the Vulkan material block defines), the lighting block upload (BuildLightingBlock), UpdateSceneLighting, PBR parameters, bind groups for the lit layout within the device limits checked in G3a (raise the required limits check if needed and say so), (2) blend states in the pipeline cache key becoming REAL: opaque, alpha blend, additive, Transparent ordering semantics as the software/Vulkan renderers define, depth write rules for transparents, (3) mid-frame SetViewProjection (the uniform ring must keep earlier draws' matrices), (4) TEXT with effects: the glyph atlas texture, the text pipeline (the G1 text.wgsl with derivatives: validated), text effects/multipass as the Vulkan text path does (look at how text effects multipass was implemented), UI/WorldCanvas/Image/Particles draws that go through the same material path, (5) TESTS in the webgpu job: Mesh/Model/Lighting/NormalMapPbr/EditModeLighting/Particles/Image/WorldCanvas/Text* goldens mirroring the Vulkan golden scenes, compared with the SOFTWARE renderer in lit mode with the documented tolerances (derive the tolerances from the Vulkan lit comparison class and measure: never loosen silently) and with Vulkan's structural/golden values where the Vulkan renderer is not in the job; zero validation errors in every test; leak/lifetime tests unchanged; (6) DOCS: the WebGPU rendering docs section (what draws now: everything the software renderer draws except what you list as unsupported; wireframe ignored with one warning per W9, no compute), conventions, golden tolerances table, docs/testing.html. Option stays OFF/CI-only (W8); code compiles as C against webgpu.h only (G4 will compile it with emdawnwebgpu: no Dawn C++ headers). Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits; GitHub only via the wrapper; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^renderer/
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Lit materials, lighting block, blending, mid-frame view/projection changes and text with effects implemented against webgpu.h only
- Lit/blend/text/UI/particles goldens pass on Dawn/lavapipe with zero validation errors and documented tolerances vs software
- Option stays OFF/CI-only; all checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: blend/depth semantics equal the software renderer, uniform ring correctness for mid-frame matrix changes, lighting block layout vs WGSL (parity test still passing), glyph atlas/text effects, tolerances derived and justified not loosened, C-API only, limits checks, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs webgpu-g3b-lit-blend-text` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-webgpu-g3b-lit-blend-text` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g3b-lit-blend-text/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-webgpu-g3b-lit-blend-text --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g3b-lit-blend-text/dev.json: {"pr":<number>,"branch":"feat-webgpu-g3b-lit-blend-text","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-webgpu-g3b-lit-blend-text` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
