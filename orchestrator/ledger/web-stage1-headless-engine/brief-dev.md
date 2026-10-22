# Task web-stage1-headless-engine: Web build stage 1: engine core headless in WebAssembly (single-threaded fallback, Emscripten filesystem), engine tests and software-renderer goldens under node (issue #155, stage 1)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-web-stage1-headless-engine.

## Issue / plan
Issue #155 stage 1 (read the issue body with `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/issues/155 --jq .body` from inside the repo) and the merged stage 0 (docs/web-build.html, cmake/N2EngineWeb.cmake, the CI job `web (Emscripten, dev profile, node tests)`, what stage 0 excluded and why: engine core NOT built because it needs window/thread/filesystem source changes). Decisions already taken by the user (do NOT revisit): web PLAYER only; software renderer first; baseline single-threaded build needs NO cross-origin isolation or special headers; threads are a later optional second artifact. SCOPE OF STAGE 1: make the ENGINE CORE (scene, GameObject, components that need no GPU/window/audio, Lua scripting with sol2/Lua, serialization, physics-free subset (PhysX is OFF on web), the software renderer library target) build for Emscripten and run headless under node: (1) a single-threaded fallback wherever the engine uses std::thread/std::mutex/std::condition_variable (the logger's per-thread buffers, the software renderer's RenderThread, the Vulkan renderer is excluded): behind N2ENGINE_PLATFORM_WEB (or a runtime/compile-time `N2ENGINE_SINGLE_THREADED`), the software renderer must render on the calling thread with output identical to the threaded path (prove with the existing software-renderer goldens: they must pass under node bit-identically); (2) filesystem: std::filesystem and file IO under Emscripten's virtual filesystem (MEMFS; NODERAWFS only for tests if needed): scene/asset loading paths used by the engine tests must work; (3) exclude by #if/CMake: GLFW/window, OpenAL, Vulkan, PhysX, editor-server/TLS, FreeType if it needs a fetch (stage 0 forced it OFF: keep), ufbx if it does not build; (4) extend the existing web CI job (do NOT rename it) to also build and run the engine tests (engine_tests) and the software-renderer tests under node, with the Emscripten version pinned by stage 0; tests that cannot run on web (threads, GPU, window, audio, PhysX, sockets) must be excluded or GTEST_SKIP'ed by an explicit, documented list (prefer CMake-level exclusion of whole files over per-test skips; no test may be silently dropped on native builds); (5) native builds must be completely unchanged: every native CI leg stays green with identical behaviour; (6) docs: update docs/web-build.html (what is now built and tested, the single-threaded fallback, filesystem notes, the exclusion list with reasons, what is still missing: stages 2-7) and docs/testing.html CI paragraph. This stage will be big: if the engine core cannot be made to build within one coherent PR, do the largest coherent subset (e.g. engine core + scene + serialization + Lua + software renderer without window) and DOCUMENT precisely what remains; do not leave the build half-configured. Nothing builds locally and the Emscripten CI job is the first real test: expect fix rounds; keep every change small and guarded. Unverified items to list: everything the Emscripten run proves (compile, link, node test run), single-thread render equality (proved only by the goldens passing), the exclusion list's completeness.

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^math/
- ^text/
- ^assetimport/
- ^engine/
- ^renderer/
- ^tests/
- ^scripts/
- ^docs/
- ^\.github/workflows/tests\.yml$
- ^external/

## Acceptance
- All 10 existing checks unchanged and green; native builds behave identically
- The web job also builds the engine core and software renderer and runs engine/renderer tests under node, with an explicit documented exclusion list
- Single-threaded fallback renders identically (goldens pass bit-identically under node)
- docs/web-build.html and docs/testing.html updated; what remains is stated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: no native behaviour change (every guard is web-only; diff of threaded paths), the single-threaded fallback is correct (no deadlocks, no dropped work, ordering preserved), test exclusions are explicit and justified and do not silently drop native tests, job name unchanged, docs match code. Unverified: the Emscripten run.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-stage1-headless-engine` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-web-stage1-headless-engine` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-stage1-headless-engine/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-web-stage1-headless-engine --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-stage1-headless-engine/dev.json: {"pr":<number>,"branch":"feat-web-stage1-headless-engine","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-web-stage1-headless-engine` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
