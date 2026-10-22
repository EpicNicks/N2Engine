# Task particles-gpu-probe: Particles PR-A: GPU capability probe, Particles health row and the physics.gpu project setting
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-particles-gpu-probe.

## Issue / plan
physx-linux-enable (#120) is merged: PhysX is ON on Linux x86_64/arm64 and in the Docker image; the engine health state names are `Running` (not Ready); PVD is only built under _WIN32 (N2ENGINE_PHYSX_HAS_PVD); the smoke script has --expect-physics. Re-read the merged PhysXBackend.cpp/hpp and engine/CMakeLists.txt first. The Windows bin.zip already has PhysXGpu_64.dll (debug+release); Linux has NO GPU library (CPU-only static libs), so on Linux physics.gpu=auto/on must report GPU Unavailable (not Failed) and never touch CUDA. Decisions and design: orchestrator/proposals/physx-particles.md (sections 2-3 and the Decisions block at the end). Scope (PR-A, CI-verifiable, no behaviour change when physics.gpu is off): (1) project setting `physics.gpu` = off | auto | on, default off, parsed next to the existing `physics` settings (engine/src/ProjectSettings.cpp ~l.160), saved/loaded/documented, validated (unknown value -> warning + off). (2) In PhysXBackend::Initialize (~l.166-216), only when physics.gpu is auto or on and the build has PhysX: probe with PxGetSuggestedCudaDeviceOrdinal, PxCreateCudaContextManager and contextIsValid(); on success create the scene with eENABLE_GPU_DYNAMICS and the GPU broadphase and sceneDesc.cudaContextManager; on any failure log one warning and build today's CPU scene unchanged. With physics.gpu=off (default) NOTHING changes: no CUDA call is made at all. `on` means a failed probe is an ERROR in the health row but the host still starts with the CPU scene. Expose `GpuStatus {Disabled|Unavailable|Ready|Failed, detail}` through the physics backend interface. (3) A `Particles` row in the engine health (the generic SubsystemStatus name/state/detail strings, protocol.json ~l.845, no protocol change): Disabled when GPU is off/unavailable with the reason, Ready when the GPU scene is up. (4) Lua `Physics.IsGpuAvailable()` (engine/src/scripting/bindings/LuaPhysicsBindings.cpp ~l.191). (5) Tests: setting parse/validate/round trip, health row Disabled on CPU-only builds and with physics.gpu=off, a fake backend test of the status mapping; nothing may require a GPU (guard with N2ENGINE_PHYSX_ENABLED and skip). (6) docs/physics.html: the setting, the probe, why GPU dynamics changes the whole rigid-body scene, the Unity-like default. Unverified (needs the user's Windows GPU PC): the actual CUDA context creation and GPU scene.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^README\.md$

## Acceptance
- All Tests-workflow jobs green (no GPU on CI: the probe is never exercised there beyond the disabled path)
- physics.gpu=off makes no CUDA call and changes no behaviour
- Health reports Particles Disabled with a reason on CPU-only builds
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: check no CUDA/GPU symbol is touched when the setting is off (including static initialisers and the Windows DLL delay-load), that failure paths never crash, and that PhysX-OFF builds (Windows PhysX OFF leg, GCC leg) still compile. Unverified: a real GPU.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs particles-gpu-probe` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-particles-gpu-probe` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/particles-gpu-probe/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-particles-gpu-probe --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/particles-gpu-probe/dev.json: {"pr":<number>,"branch":"feat-particles-gpu-probe","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-particles-gpu-probe` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
