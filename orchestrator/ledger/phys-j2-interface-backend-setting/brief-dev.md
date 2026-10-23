# Task phys-j2-interface-backend-setting: Jolt J2 (+ A1 follow-up): IPhysicsBackend additions (backend id, get/set body state, snapshot hooks), physics.backend project setting read at Init, and the three A1 low fixes
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-phys-j2-interface.

## Issue / plan
Physics investigation C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md (decisions P1-P11 ACCEPTED 2026-10-20: Jolt as a second 3D backend, backend chosen PER PROJECT via physics.backend read at Init, PhysX stays the default, strict float). Read sections 1.1, 4.2 (selection, coexistence, declaration), 4.3, 4.4 (snapshot/restore API seam additions) and the J2 line of 6.3 FIRST, plus the merged A1 (engine/src/network/*, IPhysicsBackend GetDeterminism/SupportsSnapshot/SupportsSetState, Network row), the merged J1 state (Jolt vendored: PR #195 may still be merging: this item must NOT depend on the Jolt target) and docs/physics.html. THIS ITEM IS J2 + A1 LOW FIXES. SCOPE: (1) IPhysicsBackend additions with defaults so PhysX and mocks keep compiling: a backend id/name (enum or string: 'physx', 'jolt'), body state get/set (position, rotation, linear and angular velocity, sleeping) with the PhysX implementation using setGlobalPose/setLinearVelocity/setAngularVelocity/setWakeCounter/putToSleep per the investigation (PhysX SupportsSetState becomes TRUE only if you implement and test it; if a part cannot be done safely say so and keep it false), snapshot hooks signatures per 4.4 (SaveState/RestoreState/StateHash returning 'unsupported' by default); (2) the physics.backend project setting (values physx|jolt, default physx, validated and clamped with a WARNING on a bad value like the other settings; read in Application::Init the way physics.deterministic is read), backend selection at Init: 'jolt' when no Jolt backend is compiled in is a clear logged error and a refused start for the deterministic preset or a fall-back to physx with a warning otherwise (decide per the investigation 4.2 and record as a deviation); the Network preset validation uses the SELECTED backend's GetDeterminism; health row detail names the backend; the Deterministic preset locks the backend to jolt in the preset table data (the table row exists from A1: update its value, keep the L/T/S class); (3) A1 LOWS (the user accepted them as follow-ups): the none preset must not keep an earlier simulation mode: Shutdown (or Init for none) resets the mode to None with a test; the authoritative preset's no-drop clock has no resync: add the documented default behaviour that a clock that ends a frame behind by more than a bounded amount (a named constant, e.g. 1 second of ticks) reports through the existing behind handler or resets via ResetAccumulator with a logged warning ONLY in the authoritative/deterministic modes (decide and record; None unchanged byte-for-byte), tested with the headless stepping API; fix the stale header comments Application.hpp:101 and SimulationClock.hpp:11; (4) TESTS (all legs): setting parse/clamp/default, selection logic (physx default; unavailable jolt behaviour), PhysX body get/set round trips (PhysX-only tests behind N2ENGINE_PHYSX_ENABLED), the A1 lows, the none path unchanged; (5) DOCS: docs/physics.html (the setting, selection, interface additions), docs/application.html, docs/testing.html. Nothing builds locally for the engine; CI (11 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); verify every docs sentence against the CODE; no attribution lines on commits; GitHub only via the wrapper.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^editor-server/

## Acceptance
- IPhysicsBackend additions with safe defaults; PhysX body state get/set implemented and tested or honestly left unsupported
- physics.backend setting read at Init with selection logic and honest health/preset behaviour; none path unchanged
- A1 lows fixed with tests (mode reset, bounded resync, stale comments)
- All checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: PhysX set-state calls are correct (wake/sleep/velocity semantics, no use while simulating), interface defaults keep other backends/mocks compiling, selection fallback is honest (never silently degrades a Deterministic project), none/offline path byte-for-byte unchanged, mode reset and resync correct and bounded, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs phys-j2-interface-backend-setting` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-phys-j2-interface` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/phys-j2-interface-backend-setting/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-phys-j2-interface --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/phys-j2-interface-backend-setting/dev.json: {"pr":<number>,"branch":"feat-phys-j2-interface","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-phys-j2-interface` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
