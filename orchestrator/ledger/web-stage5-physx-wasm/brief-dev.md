# Task web-stage5-physx-wasm: Web build stage 5: investigate and, if feasible, build PhysX (CPU) for WebAssembly behind the existing physics backend seam (issue #155, stage 5)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-web-stage5-physx-wasm.

## Issue / plan
Issue #155 stage 5 (read the issue body with: bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/issues/155 --jq .body, from inside the repo; also issues #156 and #157 for the custom-physics investigation and the PhysX limitations) and the merged stages 0-4: docs/web-build.html (READ ALL: the PhysX row says N2ENGINE_USE_PHYSX is OFF on the web), cmake/N2EngineWeb.cmake, web/ (budgets.json, n2-loader.js, player/), external/PhysX, the Linux/Windows PhysX CMake integration (cmake/PhysXLinuxHashes.cmake, PhysXWindowsHashes.cmake, how engine/CMakeLists.txt downloads and links PhysX, N2ENGINE_PHYSX_ENABLED), the physics backend seam (engine/include/engine/physics/IPhysicsBackend.hpp, engine/include/engine/physics/physx/*, engine/src/physics/*), proposals C:/Method Notes Obsidian/orchestrator/proposals/physx-limitations-and-determinism.md and physx-determinism-measured-numbers.md (what is known about PhysX limits, the strict-FP/SIMD work in #161/#162, the determinism options) and third-party-notices-audit-2026-10-15.md (licence/notice conventions; THIRD_PARTY_NOTICES). Decisions already taken (do NOT revisit): player only; static files only, ANY host, NO special headers (no cross-origin isolation); the single-threaded build is the baseline (the threaded artifact is stage 6, NOT this item); native builds must be completely unchanged; size budgets are enforced in CI (web/budgets.json). THIS ITEM IS INVESTIGATE-FIRST: no official PhysX WebAssembly build exists (community CPU builds do). STEP 1, INVESTIGATION (write the findings into docs/web-build.html and the PR body, with evidence: commands, versions, sizes): can the PhysX SDK version the engine uses (see the CMake: find the exact version/tag) be compiled for Emscripten as CPU-only, single-threaded (no GPU/CUDA, no PhysX threads: use the SDK's synchronous CpuDispatcher, no Windows/Linux-only APIs), static libraries, with the feature set the engine actually uses (grep engine/ for PxPhysics/PxScene/PxRigidDynamic/shapes/materials/raycast/overlap/sweep/joints/triggers/contact callbacks/CCD/character controllers etc.)? Which PhysX components can be omitted (PVD, GPU, vehicle, extensions)? Is PhysX's licence (BSD-3) and any dependency notice requirement satisfied and recorded in THIRD_PARTY_NOTICES? Where does the source/binary come from: the engine downloads prebuilt native binaries today; for the web the SDK must be BUILT from source in CI (pin a git tag/commit and a hash; state the CI cost: configure + compile minutes with ccache, cache size) or fetched as a pinned prebuilt artifact: choose the most reproducible option, justify it, and record the exact pins. What is the size cost against web/budgets.json (measure in CI: size/speed/dev profiles), and the load-time cost? Does the existing determinism/test suite for PhysX pass under node (the PhysX-only tests are currently excluded/skipped on the web): which can run under wasm+node and which cannot (compare to the native expectations: the measured numbers proposal says what differs across platforms; a golden that depends on native float behaviour must be classified, not silently loosened)? STEP 2, DECISION GATE: if the investigation shows PhysX CPU on WASM is NOT feasible within reasonable cost (build breaks that need patching the SDK heavily, size beyond budgets by a wide margin, licence blocker), STOP and report it in dev.json needsUser with the evidence and a recommendation (do not push a half-working build): the user then decides between custom physics (#156) and a size-budget change. If feasible, STEP 3, IMPLEMENT: enable N2ENGINE_USE_PHYSX on the web through the same backend seam (no changes to native behaviour), web-specific CMake that builds/fetches PhysX for Emscripten, CPU dispatcher single-threaded, assertions/error callbacks routed to the engine logger, memory behaviour sane for wasm (no big up-front allocation beyond what the budgets allow: report the heap growth settings), add the PhysX tests that can run under node to web_tests (keep exclusions only where truly impossible, each with a reason in the docs table), update web/budgets.json DELIBERATELY with the measured numbers and the reasoning (never loosen silently), keep the CI time reasonable (state the added minutes; use ccache/actions cache for the PhysX build, which must be keyed by the pin), update docs/web-build.html (stage 5: what works, what does not, determinism notes versus native, size), docs/physics.html web note, docs/testing.html and docs/web-checklist.html (concrete browser steps for physics: bodies fall and collide, raycast, performance on a stack of N bodies; list as USER-verified), and THIRD_PARTY_NOTICES. Verify every docs sentence against the CODE. Nothing builds locally; the Emscripten CI job is the test; expect fix rounds. List as UNVERIFIED everything needing a real browser (performance, frame time, determinism versus native).

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^external/
- ^web/
- ^scripts/
- ^engine/
- ^tests/
- ^docs/
- ^THIRD_PARTY_NOTICES
- ^\.github/workflows/tests\.yml$

## Acceptance
- Investigation recorded with evidence (version, feature subset, source/pins, size, licence, test classification), or a clear STOP with recommendation in needsUser if not feasible
- If feasible: PhysX CPU single-threaded builds for Emscripten behind the existing backend seam; native builds/tests unchanged
- Runnable PhysX tests pass under node; exclusions each justified; budgets updated deliberately with numbers; CI time stated
- Docs, THIRD_PARTY_NOTICES and manual browser checklist updated; browser-only items listed as unverified
- All 10 checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: native builds/tests unchanged (diff native paths and CMake conditionals), pins and hashes real and reproducible, the web PhysX build really is single-threaded CPU-only, assertions/errors routed, test exclusions each justified, budgets changed only with recorded numbers, licence notices complete, docs true vs code, checklist concrete. Unverified: real browsers, performance, determinism versus native.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-stage5-physx-wasm` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-web-stage5-physx-wasm` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-stage5-physx-wasm/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-web-stage5-physx-wasm --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-stage5-physx-wasm/dev.json: {"pr":<number>,"branch":"feat-web-stage5-physx-wasm","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-web-stage5-physx-wasm` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
