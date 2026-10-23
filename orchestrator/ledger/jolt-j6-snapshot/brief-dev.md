# Task jolt-j6-snapshot: Jolt J6: SaveState/RestoreState/HashState on the Jolt backend, parked-body contract, rollback-equality and thread-count tests
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-jolt-j6-snapshot.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/physics-backend-investigation-156.md (P1-P11 ACCEPTED): read the IPhysicsBackend snapshot interface (around lines 182-192: SupportsSnapshot(), SaveState(std::vector<std::byte>&) const between steps only, RestoreState(span), HashState() canonical in handle order), the Jolt snapshot paragraph (line ~74: StateRecorder, EStateRecorderState flags, StateRecorderFilter, SetValidating), the J6 row (line ~247), Q2 (thread-count independence is implied, not stated: J6 must PROVE it with a test), and the netcode rollback sections referenced there; then the merged J3/J4/J5 code on master (engine/src/physics/jolt/JoltBackend.cpp, ContactTracker, IPhysicsBackend.hpp, the existing HashState/SaveState stubs and their WarnOnce, PhysicsFeatures with the Snapshot requirement the Deterministic/rollback checks use, tests/engine/physics/JoltBackendTests.cpp and the typed physics suites). SCOPE: (1) JoltBackend::SupportsSnapshot() true; SaveState = Jolt SaveState(All) bytes (use a filter that skips static/inactive bodies if safe) PLUS backend bookkeeping that affects future results and events: the ContactTracker touch maps, pending Exit queues, per-body enabled flags, gravity, layer-matrix revision, any stored ColliderSpec state that is not recreated from the scene; RestoreState REFUSES (returns false and logs an Error naming the reason) if the live body set differs from the snapshot's (spawns/despawns inside the window must be parked by the caller via SetBodyEnabled(false), which keeps the BodyID: document this parked-body contract); HashState hashes body states in handle order with NaN canonicalised (independent of Jolt's byte layout); remove the snapshot WarnOnce/stub paths and declare the Snapshot feature so the rollback preset requirement is met for Jolt (update the tests/docs that expected the 'snapshot' refusal, e.g. NetworkValidationTests rollback case, HealthTests, physics.html preset paragraphs and tables, index rows); (2) tests (typed where meaningful): snapshot round trip (save, step N, restore, step N again => identical HashState and identical body transforms and identical events), rewinds of k ticks with a changed input giving the same result as a straight run, a touch begun before the snapshot and ended after restore produces the same Enter/Exit sequence as the straight run, refusal when the body set differs, parked bodies restore fine, and the same-binary 0-versus-N job-system thread count equality test (run the same scenario with a single-threaded job system and with N worker threads and assert equal HashState each tick; if the backend currently forces single-threaded, use the engine's existing option to select threads and record in the docs exactly what was proven); (3) docs/physics.html: the snapshot section verified against the code, the parked-body contract, limits (what is and is not captured), and updated Not-verified lists; docs/testing.html rows. Nothing builds locally: re-read every edit twice and trace every identifier (a failed CI costs an hour). PhysX behaviour must not change. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/

## Acceptance
- Jolt SaveState/RestoreState/HashState implemented incl. backend bookkeeping; restore refuses a different body set
- Round trip, rewind equality, touch/Exit sequence equality, refusal, parked bodies and 0-vs-N thread equality tests
- Snapshot feature declared; refusal tests and docs updated; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (concurrency/numerics): bookkeeping completeness (anything that influences future results or events must be saved), hash canonicalisation (NaN, -0, order), restore atomicity (a refused or failed restore leaves the world unchanged), thread-count test truly exercises workers, no data races with the contact listener, parked-body contract documented, feature declaration consistent with the gates, compile-level correctness of every identifier.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs jolt-j6-snapshot` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-jolt-j6-snapshot` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/jolt-j6-snapshot/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-jolt-j6-snapshot --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/jolt-j6-snapshot/dev.json: {"pr":<number>,"branch":"feat-jolt-j6-snapshot","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-jolt-j6-snapshot` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
