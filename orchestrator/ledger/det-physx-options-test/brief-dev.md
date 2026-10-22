# Task det-physx-options-test: Determinism S1 (B7, B10, F1): PhysX 0-thread + enhanced-determinism options and a same-binary determinism / 1-ulp divergence test (issue #157)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-determinism-physx-options-test.

## Issue / plan
Analysis: C:/Method Notes Obsidian/orchestrator/proposals/physx-limitations-and-determinism.md (read sections 3, 4.4 and 5 fully first: the blocker inventory B1-B10 with file/line evidence; the line numbers are from master 7222391, re-verify each against the code before editing; docs/prose must be verified against CODE). This is stage 'S1 (cheap)' of the determinism plan (issue #157): small, CI-verifiable changes that are useful even without netcode (reproducible replays and tests). HARD RULES: default behaviour of every existing project/scene/test must be UNCHANGED unless the new setting/option is switched on; no protocol change; no network presets yet (that is stage S2, not this item); no new third-party dependency. Nothing builds locally; the PR's CI (9 checks) is the test, so write tests that run on every leg and keep platform-specific expectations out of them. List unverified items honestly. No Co-Authored-By trailer and no 'Generated with Claude Code' footer. SCOPE: (a) add an OPT-IN physics setting (default OFF, no behaviour change) that sets PxSceneFlag::eENABLE_ENHANCED_DETERMINISM and uses 0 CPU dispatcher worker threads (PhysXBackend.cpp near :323 and :337); refuse/warn clearly if combined with GPU physics (not supported by the flag). (b) F1 test (PhysX legs only, GTEST_SKIP elsewhere following how other PhysX tests are guarded): build the same scenes twice in two fresh PhysX scenes in one process with the option on and assert BIT-IDENTICAL state hashes after N steps (free fall, box stack, a ~100-body pile, a ragdoll if the engine has one); then a 1-ulp perturbation experiment per proposals section 3: perturb one body's initial position by 1 ulp and PRINT (not assert) the first step at which any body differs by >1e-3 m, so the divergence numbers for real PhysX are recorded in CI logs; also print a hash of the final state so Windows vs Linux hashes can be compared by hand (assert nothing cross-platform). Also test thread count 0 vs 4 equality and print the result without asserting if it differs (the vendor docs do not promise it): the report's 'could not verify' list says this is unmeasured. Docs: record the findings procedure in docs/testing.html; do not state any measured number you did not see in a CI log (the PR's own CI output is where they appear; say so).

## Allowed paths (regex, repo-relative)
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/

## Acceptance
- Default physics behaviour unchanged
- Option sets enhanced determinism + 0 threads and warns with GPU physics
- Same-binary identical-hash assertions pass on PhysX legs; divergence and hash values are printed in CI logs
- Docs describe the option and where to read the numbers
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: asserted vs merely printed results are separated correctly (no assertion on cross-platform or unpromised behaviour), hashing covers positions, rotations and velocities bit-exactly, the test is skipped cleanly on non-PhysX legs, default settings untouched. After CI, the orchestrator reads the printed divergence/hash numbers from the job logs and reports them to the user.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs det-physx-options-test` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-determinism-physx-options-test` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/det-physx-options-test/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-determinism-physx-options-test --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/det-physx-options-test/dev.json: {"pr":<number>,"branch":"feat-determinism-physx-options-test","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-determinism-physx-options-test` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
