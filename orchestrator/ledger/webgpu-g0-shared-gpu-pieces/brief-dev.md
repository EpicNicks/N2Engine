# Task webgpu-g0-shared-gpu-pieces: WebGPU G0: move the backend-neutral GPU pieces out of the Vulkan tree (shader interface table, lighting block, block material) and the cross-backend test helpers to common locations; no behaviour change
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: refactor-webgpu-g0-shared-gpu.

## Issue / plan
USER DECISIONS (2026-10-20, final, do NOT revisit): the design in C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md is accepted with ALL recommendations W1-W10: hand-written WGSL for the three built-in shaders tied to the shared interface table by a parity test and validated by Tint (and naga) in CI (W1); WebGPU goldens run natively on Dawn over lavapipe in a new NON-REQUIRED Linux CI job (W2, G2 and later); one wasm containing both renderers chosen at load (W3); WebGPU is the automatic default when the browser offers it with a ?renderer= override (W4); device loss = one silent re-acquire then a 'continue with software' button (W5); emdawnwebgpu with the device acquired in loader JS before callMain, no ASYNCIFY (W6); devicePixelRatio with a 2560 cap (W7); the native WebGPU backend exists only for the CI harness, option OFF by default, not selectable in lua_project or the editor host (W8); v1 = built-in shaders only, wireframe ignored with one warning, no compute, particles/soft bodies on CPU paths (W9); G0-G2 now, G4 after the stage 5 PR #184 merges (W10). Read C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md FULLY (sections 3-7 and the staged plan) before starting. THIS ITEM IS PR G0 ONLY, a PURE MOVE/REFACTOR with NO behaviour change. Per the design: renderer/vulkan VulkanShaderInterface.hpp and VulkanLighting.hpp contain no Vulkan code, and VulkanMaterial's interface uses no Vulkan types: (1) move VulkanShaderInterface.hpp -> a common GPU header (e.g. renderer/common/include/.../GpuShaderInterface.hpp: choose the right existing common location by reading renderer/ layout and CMake; rename the symbols/namespace only where the Vulkan prefix would be wrong in a shared header, keep everything else identical), VulkanLighting.hpp -> common (GpuLighting.hpp), and lift VulkanMaterial's backend-neutral part into a shared BlockMaterial (the Vulkan material keeps only its Vulkan-specific parts and derives from or wraps the shared one: pick the least invasive shape that keeps the Vulkan backend's code and results identical); (2) move CrossBackendDiff.hpp and BlendReference.hpp (find them under tests/renderer/) to tests/renderer/common/ so a later WebGPU test target can include them, updating includes and CMake; (3) update every include and CMake target in renderer/, engine/ and tests/; the Vulkan backend must still be selectable and every existing Vulkan, software and OpenGL test must stay green with UNCHANGED results and unchanged test names (CI: the linux Clang legs EXECUTE the lavapipe Vulkan tests: that is the oracle); native Windows builds without Vulkan (the N2ENGINE_RENDERER_VULKAN option OFF leg, GCC 14 leg) must still configure and build since the moved headers must not need Vulkan; (4) docs: update any docs page that names the moved files or the Vulkan-specific location (grep docs/ and comments for the old paths: docs/rendering*, vulkan pages, testing.html) and add one short sentence in the rendering docs that these pieces are shared by GPU backends; verify against the code; (5) NO new code paths, no new targets besides moving files, no WebGPU code yet. List any symbol rename as a deviation. Nothing builds locally; CI (10 checks) is the test; expect fix rounds on include/CMake mistakes: re-read every CMake and include edit twice.

## Allowed paths (regex, repo-relative)
- ^renderer/
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/

## Acceptance
- Shader interface, lighting block and block material live in common locations; Vulkan uses them with unchanged behaviour
- CrossBackendDiff and BlendReference live in tests/renderer/common; all includes/CMake updated
- Every existing Vulkan, software and OpenGL test passes unchanged, including legs with Vulkan OFF
- All 10 checks green; docs updated for moved paths
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: it is a pure move (git diff -M shows renames with only include/namespace edits; no logic change), Vulkan-OFF and non-Vulkan legs still build (the moved headers include nothing from Vulkan), no stale old-path references anywhere (grep), test names unchanged, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs webgpu-g0-shared-gpu-pieces` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref refactor-webgpu-g0-shared-gpu` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g0-shared-gpu-pieces/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head refactor-webgpu-g0-shared-gpu --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g0-shared-gpu-pieces/dev.json: {"pr":<number>,"branch":"refactor-webgpu-g0-shared-gpu","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/refactor-webgpu-g0-shared-gpu` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
