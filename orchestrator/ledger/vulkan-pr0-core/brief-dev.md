# Task vulkan-pr0-core: Vulkan PR 0: core initialisation, offscreen target, readback, shutdown and a lavapipe-backed CI test harness (issue #43, stage 0)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-vulkan-core-harness.

## Issue / plan
Approved by the user 2026-10-14 (proposal with the user's decisions: C:/Method Notes Obsidian/orchestrator/proposals/vulkan-backend.md; read it FULLY first: section 1 inventory (file:line of every stub), 2 contract, 3 scope, 4 verification strategy, 5 stage table, 6 deps, 7 risks, and 'Decisions (user, 2026-10-14)'). This item is stage 'PR 0 Core + harness' ONLY; later stages (A resources, B shaders/pipeline, C materials, D queues/blending, E text, F window/swapchain/present) are separate items: do not start them. Scope: (1) rewrite VulkanRenderer initialisation (renderer/ Vulkan files; the proposal's inventory says the existing init is unreachable and buggy: structs not zero-initialised, inverted surface-format test, wrong depth layout, FindSupportedFormat stub returning UNDEFINED, CheckValidationLayerSupport stub returning false, validation hard-coded on at VulkanRenderer.hpp:271) so that headless `Initialize(nullptr)` works: instance with real layer/extension checks, optional validation (off by default in Release; on when N2_VULKAN_VALIDATION=1 and always on in the test fixture; a debug messenger that COUNTS errors/warnings), physical device selection, logical device and queue, offscreen colour target (R8G8B8A8_UNORM) and depth target (FindSupportedFormat D32/D24S8), command pool and buffers, 2 frames in flight with fences, BeginFrame clears to the requested colour, EndFrame submits, ReadFramebuffer reads the offscreen colour back, SetRenderTargetSize recreates targets, full Shutdown that destroys everything (validation must report no leaked objects at device destroy), a lifetime token so resources outliving the renderer are safe. Keep the IRenderer interface unchanged (the proposal verified no change is needed); every resource/draw path that stage A-E will implement stays a documented stub that fails loudly and cleanly (logs once, returns nullptr/false) without crashing; the swapchain/present path stays out of scope (stage F): Present may be a no-op with a clear message in headless mode. Respect the user's decisions: always render offscreen; N2ENGINE_RENDERER_VULKAN CMake option so Vulkan can be compiled out (default ON as today: check how the Vulkan SDK is found today on Windows and Linux in CMake and tests.yml, and do not break any current leg); the editor host keeps refusing --renderer vulkan; OpenGL stays the default renderer. (2) CI: add lavapipe + validation layers + glslang-tools to the Linux jobs (the three Linux apt lines in .github/workflows/tests.yml and the Docker build stage; the proposal marks package names [M]: verify against Ubuntu 24.04 noble: mesa-vulkan-drivers, vulkan-validationlayers (or vulkan-validation-layers), glslang-tools; use VK_DRIVER_FILES to pin the lavapipe ICD per architecture, paths are [M]: make the harness robust: discover the ICD instead of hardcoding if feasible; the arm64 leg exists too). The Windows legs only COMPILE the Vulkan code (they already have the SDK) and the tests GTEST_SKIP with a clear reason when no Vulkan device exists, so a Windows machine without a driver still passes. (3) Tests (renderer tests dir, mirror how renderer/software tests are organised and registered; read tests/CMakeLists first): a gtest fixture that creates the headless renderer with validation on, fails the test on any validation error and on undestroyed objects at teardown, and GTEST_SKIPs without a Vulkan device; tests: clear colour reads back EXACTLY at several sizes (incl. odd sizes like 1x1, 7x5, 64x64), clear colour changes between frames, SetRenderTargetSize then readback, re-initialise after Shutdown, double Shutdown is safe, Initialize failure paths leave no leaks, zero validation errors across the whole suite; in the CI Linux required leg the tests must actually EXECUTE on lavapipe (assert in a CI-only way that they did not all skip: e.g. an env var set only in the Linux CI jobs makes 'no device' a failure instead of a skip). (4) Docs: docs/ page for the renderer or build (check which pages mention Vulkan; keep edits small): how to build, the N2ENGINE_RENDERER_VULKAN option, N2_VULKAN_VALIDATION, how to run the Vulkan tests with lavapipe locally, current status (stage 0: headless clear/readback only; draws nothing until stages A-E). Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`). If something in the proposal turns out wrong (package names, ICD paths, extension availability on lavapipe), adapt minimally, list it in `deviations`, and say so; do not guess silently. Vulkan on lavapipe cannot be tested locally: write defensively and report uncertainty in `unverified`.

## Allowed paths (regex, repo-relative)
- ^renderer/
- ^engine/
- ^tests/
- ^docs/
- ^cmake/
- ^CMakeLists\.txt$
- ^\.github/workflows/tests\.yml$
- ^deploy/
- ^Dockerfile$
- ^THIRD_PARTY_NOTICES\.md$

## Acceptance
- All Tests-workflow jobs green; the Vulkan tests EXECUTE on lavapipe in the Linux legs (not all skipped)
- Headless Initialize/clear/readback/resize/shutdown work with zero validation errors and no leaks
- IRenderer interface unchanged; OpenGL default and --renderer vulkan refusal unchanged; Windows legs only compile and skip
- Docs describe status, option and validation switch
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: check object lifetimes and destruction order (validation clean), 2-frames-in-flight sync (fences, no use-after-free), image layout transitions for the offscreen/depth targets and readback, staging/readback memory barriers, error paths free everything, no UB from uninitialised structs, the lifetime token, that the tests cannot silently all-skip in the required Linux leg, CMake option compiles Vulkan out cleanly. Unverified: real GPU drivers, swapchain/present (stage F), performance.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs vulkan-pr0-core` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-vulkan-core-harness` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/vulkan-pr0-core/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-vulkan-core-harness --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/vulkan-pr0-core/dev.json: {"pr":<number>,"branch":"feat-vulkan-core-harness","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-vulkan-core-harness` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
