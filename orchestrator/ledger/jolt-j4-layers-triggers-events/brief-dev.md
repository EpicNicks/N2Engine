# Task jolt-j4-layers-triggers-events: Jolt J4: layers and the collision matrix, triggers (sensor strategy from a spike), contact/trigger events through a backend-neutral ContactTracker shared with PhysX; LayerPhysicsTests and PhysicsOrderTests typed over both backends
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-jolt-j4-layers-triggers-events.

## Issue / plan
Physics investigation C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md (P1-P11 ACCEPTED): read 4.3 (the Trigger collider, PhysicsMaterial, Layers and Events rows) FULLY, 4.4, the J4 row of 6.3, 7 risks and the open question Q3 (contact impulses). MERGED: J1 vendor, J2 seam, J3 JoltBackend core (#205: read engine/src/physics/jolt/*, its docs/physics.html section with the PhysX/Jolt difference table, the typed tests it introduced). The PhysX backend (engine/src/physics/physx/*) is the behavioural reference: read its TouchMap/event logic, the layer matrix handling and the re-filter Exit/Enter semantics FIRST. SCOPE (J4 only; queries are J5 running in parallel, snapshots J6): (1) LAYERS: ObjectLayer = engine layer (+ static/moving bit), ObjectLayerPairFilter reading the engine matrix, 2 broad-phase layers; a matrix change swaps the filter and re-validates touching pairs reproducing PhysX's Exit/Enter semantics; (2) TRIGGERS: SPIKE first between (a) two Jolt bodies per mixed body (solid + sensor body moved with it) and (b) per-sub-shape filtering in OnContactValidate: pick by correctness and determinism, document the choice and why in the PR body and docs (the user accepts it); sensors excluded from mass; QueryTriggers semantics kept for J5; (3) EVENTS: factor the PhysX backend's touch bookkeeping into a backend-neutral ContactTracker used by BOTH backends (PhysX behaviour must not change: its tests pass unchanged); the Jolt ContactListener (multi-threaded) pushes into a mutex-protected buffer; after Update sort by (handle pair, sub-shape pair) and fold into the ContactTracker so event ORDER is deterministic; Enter/Stay/Exit per body pair, contact points; per-collider friction/restitution applied in OnContactAdded/Persisted via ContactSettings combined values (combine rule as PhysX); normalImpulse: implement what Jolt allows and DOCUMENT the gap (Q3) as a deviation; disabling a body ends its pairs with Exit; (4) TESTS: LayerPhysicsTests and PhysicsOrderTests become typed tests over both backends (behaviour differences recorded in the docs table and dev.json deviations, never silently skipped), plus trigger tests for mixed bodies, event order determinism (same scene twice -> identical event sequence), matrix change mid-contact; (5) DOCS: docs/physics.html (layers, triggers strategy, events, the impulse gap, updated difference table), docs/testing.html. FEATURE DECLARATION: a parallel item (phys-selection-lows) adds a backend-declared set of implemented feature groups (layers, triggers, events, queries) that the Deterministic preset validation reads; if it is on master when you merge origin/master, set the bits for what YOUR item implements in JoltBackend and adjust its tests; if it is not, say so in the PR body. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved (other Jolt items run in parallel: resolve conflicts keeping both sides); your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Layers/matrix with PhysX-equal re-filter semantics on Jolt
- Triggers via the spiked strategy, documented
- Shared ContactTracker; PhysX behaviour unchanged; deterministic event order on Jolt
- LayerPhysicsTests and PhysicsOrderTests typed over both backends
- All checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (concurrency/determinism): ContactListener thread safety, sort key completeness (ties), PhysX unchanged after the refactor, matrix-change semantics, sensor mass exclusion, documented gaps true. Mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs jolt-j4-layers-triggers-events` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-jolt-j4-layers-triggers-events` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/jolt-j4-layers-triggers-events/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-jolt-j4-layers-triggers-events --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/jolt-j4-layers-triggers-events/dev.json: {"pr":<number>,"branch":"feat-jolt-j4-layers-triggers-events","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-jolt-j4-layers-triggers-events` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
