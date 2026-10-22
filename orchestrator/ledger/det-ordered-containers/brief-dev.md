# Task det-ordered-containers: Determinism S1 (B3, B9): index-ordered iteration in coroutines, collision refiltering and touch maps (issue #157)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-determinism-ordered-containers.

## Issue / plan
Analysis: C:/Method Notes Obsidian/orchestrator/proposals/physx-limitations-and-determinism.md (read sections 3, 4.4 and 5 fully first: the blocker inventory B1-B10 with file/line evidence; the line numbers are from master 7222391, re-verify each against the code before editing; docs/prose must be verified against CODE). This is stage 'S1 (cheap)' of the determinism plan (issue #157): small, CI-verifiable changes that are useful even without netcode (reproducible replays and tests). HARD RULES: default behaviour of every existing project/scene/test must be UNCHANGED unless the new setting/option is switched on; no protocol change; no network presets yet (that is stage S2, not this item); no new third-party dependency. Nothing builds locally; the PR's CI (9 checks) is the test, so write tests that run on every leg and keep platform-specific expectations out of them. List unverified items honestly. No Co-Authored-By trailer and no 'Generated with Claude Code' footer. SCOPE: replace pointer-keyed hash iteration order with creation-index order in simulation paths: CoroutineScheduler::_coroutines (unordered_map<GameObject*,...>), PhysXBackend::RefreshCollisionMatrix grouping (unordered_map<PxRigidActor*,...>), the TouchMap wake loop, and any other pointer-keyed container iterated in a sim path you find (grep; also look at the CPU backends, which already step in creation order as the pattern to follow: CpuSoftBodyBackend.hpp:120). Keep lookup complexity reasonable (index-ordered map or vector + id; justify). No behaviour change except the order becomes deterministic: existing tests must pass unchanged. New tests: coroutine execution order over N objects equals creation order and is identical across 100 randomised create/destroy sequences with different allocation patterns; the refilter order is creation order (use a fake/observable seam; PhysX tests only run on PhysX legs, keep the coroutine test backend-independent). Docs: one paragraph stating the ordering guarantee.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/

## Acceptance
- Coroutines run in creation order regardless of allocation addresses (test)
- Refilter and touch-map iteration is creation-ordered
- All existing tests pass unchanged
- Ordering guarantee documented
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: every pointer-keyed container iterated in a sim path is covered or explicitly listed as deliberately left; no per-frame performance regression (state the complexity); coroutine add/remove during iteration semantics unchanged.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs det-ordered-containers` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-determinism-ordered-containers` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/det-ordered-containers/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-determinism-ordered-containers --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/det-ordered-containers/dev.json: {"pr":<number>,"branch":"feat-determinism-ordered-containers","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-determinism-ordered-containers` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
