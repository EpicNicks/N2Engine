# Task phys-lows-j4-j5: Physics follow-ups: trigger Exit after a speculative-only contact, Persisted/re-Enter docs, RaycastAll tie order, PhysX single-hit query cost, no-backend log level, design-jolt-queries index row
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-phys-lows-j4-j5.

## Issue / plan
Follow-ups of merged #211, J4 (#213), J5 (#212); the user accepted these LOW review findings as a follow-up. Verify each against the CODE on master first (git grep to locate), fix only what is real, and say in the report which you did not change and why. (1) J4: in the Jolt contact folding (JoltBackend.cpp FoldContacts / the presentPairs set) presentPairs is filled for every present report including a trigger contact that is only inside the speculative distance (touching false); the old key of a tracked trigger touch is then erased silently while the new key's info stays reported=false, so the tracker keeps the trigger touch and no later parted/ended path can send its Exit: fix so a trigger touch is never left without an Exit path (only count a pair present when it is actually touching, or carry the reported flag over to the new key) with a test (a trigger starting speculative-only then touching then leaving: exactly one Enter and one Exit). (2) J4 docs/physics.html: the sentence 'every Added, the Persisted of trigger contacts, every Removed' no longer describes the listener (solid Persisted reports of untracked contacts are buffered too) and nothing says that a touch begun again after SetIsTrigger or a collision-matrix change starts with an Enter that carries no contact points, impulse or relativeVelocity: make the docs true against the code. (3) J5: RaycastAll orders by exact float distance with no kQueryTieTolerance, unlike the single-hit pick (QueryUtil): near-equal hits are ordered by float noise: apply the same tolerance + handle tie-break ordering as the single-hit pick on both backends with a test. (4) J5: every PhysX single-hit query (Raycast::Single/Any, SphereCast, CapsuleCast) now runs a second touching scene query and value-initialises a 1024-entry hit buffer on the heap: avoid the per-query heap allocation of the 1024-entry buffer (reuse a member scratch buffer or stack/small buffer) and the second query where it is not needed, WITHOUT changing results; keep the existing tests passing. (5) #211: a build with no backend at all (web leg PHYSX=OFF and JOLT off) logs an ERROR on every start because the hosts' PHYSX request is 'refused'; before it was an Info 'Built without PhysX': when the build has NO backend compiled in, log at Info/Warn level with a clear message instead of an Error (keep Error for a real refusal such as Deterministic needing features); update the HealthTests/docs strings accordingly. (6) docs/index.html design-decisions table has no row for physics.html#design-jolt-queries (a callout for the user to accept whose decisions 5 and 6 were rewritten at the merge): add the row in the table's existing style, true against physics.html. Nothing builds locally; re-read every edit twice and trace identifiers. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/

## Acceptance
- Trigger touch always has an Exit path (test)
- RaycastAll tie ordering matches single-hit on both backends (test)
- No per-query 1024-entry heap allocation or redundant second query; results unchanged
- No-backend build logs below Error; docs and index row true; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (concurrency/numerics): the speculative-contact fix cannot produce a double Enter/Exit or lose an Exit, tie tolerance semantics identical on PhysX and Jolt, the scratch buffer is not shared across threads or reentrant calls, no behaviour change in query results, compile-level correctness of every identifier.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs phys-lows-j4-j5` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-phys-lows-j4-j5` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/phys-lows-j4-j5/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-phys-lows-j4-j5 --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/phys-lows-j4-j5/dev.json: {"pr":<number>,"branch":"fix-phys-lows-j4-j5","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-phys-lows-j4-j5` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
