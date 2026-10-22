# Task particles-component-cpu: Particles PR-B: ParticleSystem component, IParticleBackend seam, CPU backend and billboard rendering
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-particles-component-cpu.

## Issue / plan
particles-gpu-probe (#122) is merged: `physics.gpu` off|auto|on (default off), GpuStatus via IPhysicsBackend, the Particles health row (states Running/Disabled/Failed), Physics.IsGpuAvailable(); on Linux the GPU is Unavailable. Re-read the merged code (engine/include/engine/physics/GpuStatus.hpp, IPhysicsBackend.hpp, PhysXBackend, Application.cpp health rows) and the proposal first. The health Particles row from PR-A reflects the PhysX GPU probe only; with this item it must also say which backend a ParticleSystem is using when one exists (CPU or PhysX) without breaking PR-A tests. Decisions: orchestrator/proposals/physx-particles.md (Decisions block): one ParticleSystem component with `backend: Auto | PhysX | CPU`; Auto = PhysX when the GPU probe is Ready, else CPU; CPU billboards rendered through UpdateMesh; no protocol change (backend is an ordinary component field). Scope (PR-B, fully CI-verifiable, runs headless): (1) engine/include/engine/physics/IParticleBackend.hpp: CreateSystem(desc)->handle, Spawn(handle, positions, velocities), SetParams, Destroy, CopyPositions(handle, std::vector<Vec4>&), obtained from IPhysicsBackend::GetParticleBackend() (nullptr by default). (2) A CPU particle backend (no PhysX): emitter (rate, burst, lifetime, start speed/size/color ranges, gravity scale, cone/sphere shape), integration in the fixed step, simple collision against the physics world via raycast/sphere checks only if cheap and optional (otherwise ground plane only; document), deterministic with a seed, capped max particles with a clear warning. (3) ParticleSystem component modelled on Light/TextRenderer: fields, serialization, reflection/inspector (no protocol change; verify GetComponentTypes exposes it and an enum field `backend`), registration, Lua access (engine/src/scripting/bindings/LuaComponentBindings.cpp ~l.109 pattern) plus Play/Stop/Clear/EmitBurst(n), play-mode rules (particle state is never serialized, exit play clears, pause freezes via timeScale 0). (4) Rendering: camera-facing billboards rebuilt on the CPU each frame through UpdateMesh on the OpenGL and software renderers (Vulkan is still stubs: no-op there with a one-time warning), additive/alpha blend setting, texture optional. The `backend` selection: PhysX with no PhysX backend/GPU falls back to CPU with one warning (health row from PR-A stays truthful); the PhysX backend itself is PR-C and is NOT in this item. (5) Tests: simulation determinism, caps, serialization round trip, Lua API, backend selection/fallback, billboard mesh generation, headless run (no window), software-renderer golden image if the existing test pattern supports it. (6) docs/ (a new particles section/page linked from the docs index), README mention.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^renderer/
- ^tests/
- ^docs/
- ^README\.md$
- CMakeLists\.txt$

## Acceptance
- All Tests-workflow jobs green on all legs including headless Linux (particles run on the CPU backend there)
- A scene with a ParticleSystem round-trips through save/load and play mode
- backend=PhysX without a GPU falls back to CPU with one warning
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Draft. Reviewer: Lua API shape consistency, no protocol.json change, no per-frame heap churn beyond the documented mesh rebuild, determinism, no GPU assumption. Unverified: OpenGL rendering on a real GPU (user's PC), Vulkan renderer (stubs).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs particles-component-cpu` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-particles-component-cpu` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/particles-component-cpu/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-particles-component-cpu --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/particles-component-cpu/dev.json: {"pr":<number>,"branch":"feat-particles-component-cpu","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-particles-component-cpu` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
