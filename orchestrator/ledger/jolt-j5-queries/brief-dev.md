# Task jolt-j5-queries: Jolt J5: raycast, raycast-all and sphere cast on Jolt; CapsuleCast and overlap queries on both backends; Lua Physics additions
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-jolt-j5-queries.

## Issue / plan
Physics investigation C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md (P1-P11 ACCEPTED): read 4.3 (the Queries row) FULLY, 4.4 (CapsuleCast/Overlap seam lines, R9) and the J5 row of 6.3. MERGED: J1-J3 (JoltBackend core #205). Read IPhysicsBackend.hpp, the PhysX query implementation, the Raycast API and the Lua Physics bindings (and engine-api.lua / LuaLS annotations if they exist) FIRST. SCOPE (J5 only; layers/triggers/events are J4 running in parallel): (1) Jolt: Raycast closest (NarrowPhaseQuery::CastRay with a custom collector, ties broken by handle), RaycastAll sorted by (distance, handle), SphereCast via CastShape; NEVER BroadPhaseQuery (not deterministic); QueryTriggers::Ignore via a body filter on sensors (if J4's sensor strategy is not on master yet, implement the filter on the current trigger representation and leave a note); layer masks honoured; (2) NEW on BOTH backends: CapsuleCast and Overlap (sphere/box/capsule) per the seam, PhysX via its sweep/overlap API, results sorted deterministically by handle; (3) Lua Physics.* additions for the new queries with argument validation, mirroring the existing Physics.Raycast binding style, annotations updated; (4) TESTS: query tests typed over both backends (hits, misses, sorted results, ties, layer masks, trigger ignore/include, zero-length and degenerate inputs, starting inside a shape), Lua binding tests; (5) DOCS: docs/physics.html queries section, docs/scripting-lua.html for the Lua API, docs/testing.html. FEATURE DECLARATION: a parallel item (phys-selection-lows) adds a backend-declared set of implemented feature groups (layers, triggers, events, queries) that the Deterministic preset validation reads; if it is on master when you merge origin/master, set the bits for what YOUR item implements in JoltBackend and adjust its tests; if it is not, say so in the PR body. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved (other Jolt items run in parallel: resolve conflicts keeping both sides); your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$

## Acceptance
- Raycast/RaycastAll/SphereCast on Jolt via NarrowPhaseQuery with deterministic ordering
- CapsuleCast and Overlap on both backends
- Lua Physics additions with tests
- Query tests typed over both backends; all checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: deterministic ordering and tie-breaks, no BroadPhaseQuery, PhysX sweep semantics (initial overlap), Lua argument validation, docs true. Mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs jolt-j5-queries` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-jolt-j5-queries` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/jolt-j5-queries/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-jolt-j5-queries --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/jolt-j5-queries/dev.json: {"pr":<number>,"branch":"feat-jolt-j5-queries","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-jolt-j5-queries` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
