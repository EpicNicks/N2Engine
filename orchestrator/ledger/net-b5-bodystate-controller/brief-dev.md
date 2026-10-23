# Task net-b5-bodystate-controller: Netcode B5: BodyState get/set on IPhysicsBackend (PhysX and Jolt), capsule cast/overlap check, engine-owned character controller
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-net-b5-bodystate-controller.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/netcode-runtime-design.md (row B5 line ~299; the prediction/controller and BodyState sections; decisions N1-N15) and C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md (IPhysicsBackend interface sections: BodyState get/set, teleport, sleep; character-controller notes). Read the merged physics code first: IPhysicsBackend.hpp, PhysXBackend, JoltBackend (J3-J6: layers, triggers, events, queries incl. CapsuleCast/Overlap, SaveState), ContactTracker, the Rigidbody/Collider components, existing character-related code if any. SCOPE: (1) BodyState (position, rotation, linear and angular velocity, sleeping flag) get/set on IPhysicsBackend with implementations on PhysX AND Jolt (teleport semantics: setting state wakes or sleeps as asked, does not generate spurious trigger/collision events beyond what the design says, kinematic bodies handled, disabled/parked bodies refuse or queue per contract), exact-roundtrip tests on both backends (typed suites); (2) verify capsule cast and overlap exist on both backends from J5 (they should: if anything is missing add it); (3) an engine-owned CharacterController component/system (capsule-based, kinematic move with collide-and-slide via the backend's capsule sweeps, ground detection, step offset, slope limit, jump/gravity integration, deterministic given the same input and the fixed tick, using only float math through the engine's deterministic math helpers where those exist), exposed to C++ and Lua (Lua binding in the same style as Rigidbody/Physics queries), with an input struct (move vector, jump) and state (position, velocity, grounded) that can be saved/restored and set from a server correction (this is what B6 prediction will use); (4) tests: BodyState round trip on both backends, controller on flat ground, slopes, steps, walls (slide), falling and jumping, ground detection, determinism (same inputs => identical state hash across two runs and on both backends), corner cases (zero move, tiny capsule, penetration recovery); (5) docs/physics.html (and scripting-lua.html for the Lua API) verified against the code. No networking in this item. PhysX behaviour for existing features must not change. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/
- ^lua_project/
- ^[^/]*\.lua$

## Acceptance
- BodyState get/set on PhysX and Jolt with round-trip tests
- CharacterController with sweeps, ground, step, slope, jump; C++ and Lua API
- Deterministic same-input tests on both backends; edge cases covered
- Docs true; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (numerics): no NaN/inf propagation, penetration recovery bounded (no infinite loops), event side effects of teleport, determinism (no unordered iteration, one RNG draw per statement, no uninitialised state), save/restore completeness of controller state, Lua binding argument validation, compile-level correctness.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs net-b5-bodystate-controller` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-net-b5-bodystate-controller` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/net-b5-bodystate-controller/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-net-b5-bodystate-controller --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/net-b5-bodystate-controller/dev.json: {"pr":<number>,"branch":"feat-net-b5-bodystate-controller","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-net-b5-bodystate-controller` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
