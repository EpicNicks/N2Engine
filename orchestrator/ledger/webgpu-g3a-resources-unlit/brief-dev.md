# Task webgpu-g3a-resources-unlit: WebGPU G3a: meshes (with the in-frame UpdateMesh rule), textures, pipeline and bind-group caches, uniform ring, DrawMesh/IndexRange/DrawObjects and the unlit shader on Dawn/lavapipe with goldens and a strict cross-diff against the software renderer
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-webgpu-g3a-resources-unlit.

## Issue / plan
WebGPU design C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md (W1-W10 ACCEPTED 2026-10-20). G0 (#188 shared pieces), G1 (#190 WGSL + validation) and G2 (#196 native harness: WebGpuRenderer core, Dawn prebuilt, 'webgpu (Dawn, lavapipe)' non-required job, error counting) are merged. Read the design sections on resources/bind groups/pipeline/uniform handling, the G3a row, the 'must change for WebGPU' list (row_major matrices: transpose-on-upload convention from G1; separate texture/sampler; mipmaps by CPU since blit is not core; in-frame UpdateMesh hazard needs a fresh buffer), the merged G2 code (renderer/src/webgpu/*) and the Vulkan backend's resource code as the template (renderer/src/vulkan/*: VulkanMesh/Texture/Pipeline caches, BlockMaterial, ApplyUniforms), GpuCache lifetime handling, the unlit WGSL and the shared GpuShaderInterface table. THIS ITEM IS G3a ONLY (unlit; lit/blending/text are G3b): (1) meshes: create/update/destroy with vertex/index buffers, the IN-FRAME UpdateMesh rule (a mesh already drawn earlier in the same unsubmitted frame gets a fresh buffer so earlier draws keep the old vertices), index ranges, (2) textures: RGB->RGBA conversion, sRGB format choice, samplers, CPU-generated mips, upload via queue writes with the 256-byte row alignment handled, (3) pipeline cache keyed by (shader, vertex layout, render state: cull/depth/blend placeholder), bind-group cache, per-draw uniform ring buffer honouring WebGPU's minUniformBufferOffsetAlignment and limits, the transpose-on-upload convention for matrices, (4) DrawMesh/DrawMeshIndexRange/DrawObjects with the unlit material, depth test/write, culling, clear/viewport/scissor as the interface requires, (5) lifetime: resources recreate through the GpuCache lifetime token on device loss like the Vulkan backend (test with the device-lost injection if Dawn offers it, else document), no leaks over create/destroy cycles, zero WebGPU validation errors in every test (the G2 counter), (6) TESTS in the existing webgpu job: resource tests mirroring Vulkan's (create/update/destroy, in-frame update rule, mip sizes, texture upload correctness with readback), unlit draw goldens (flat colour quads, textured quad with nearest and linear sampling, cull front/back, depth test ordering, IndexRange) with a STRICT cross-diff against the SOFTWARE renderer using the shared CrossBackendDiff helpers (find the right tolerance per case: exact for flat colours; document any tolerance), compare also against Vulkan goldens where they exist, (7) DOCS: the WebGPU rendering docs section from G2 extended (what draws now, what does not: lit/blend/text G3b, Emscripten G4), the conventions, the goldens and tolerances, docs/testing.html. Option stays OFF by default and CI-only (W8); do not touch the Emscripten/web build; the new code must compile as plain C against webgpu.h only (no Dawn C++ headers) so G4 can compile it with emdawnwebgpu. Nothing builds locally for the engine; CI (11 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); verify every docs sentence against the CODE; no attribution lines on commits; GitHub only via the wrapper.

## Allowed paths (regex, repo-relative)
- ^renderer/
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Meshes (in-frame update rule), textures, pipeline/bind-group caches, uniform ring and unlit draws implemented against webgpu.h only
- Resource tests and unlit goldens pass on Dawn/lavapipe with zero validation errors; strict cross-diff vs software documented
- Lifetime/device-loss recreation and leak tests
- Option stays OFF/CI-only; all required checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: uniform alignment/limits, matrix transpose applied once, the in-frame UpdateMesh rule really prevents earlier draws reading new data, row-pitch handling, sRGB/format choices vs software renderer, pipeline cache key complete, resource lifetime/recreation, zero validation errors, C-API only (nothing G4 cannot compile), goldens meaningful (fail on regressions), docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs webgpu-g3a-resources-unlit` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-webgpu-g3a-resources-unlit` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g3a-resources-unlit/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-webgpu-g3a-resources-unlit --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g3a-resources-unlit/dev.json: {"pr":<number>,"branch":"feat-webgpu-g3a-resources-unlit","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-webgpu-g3a-resources-unlit` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
