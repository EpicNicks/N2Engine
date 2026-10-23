# Task tests-eval-order-docs-lows: Split multiple RNG calls out of single argument lists in two older test files; drop the hard-coded per-executable test counts from docs/web-build.html Stage 1
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-tests-eval-order-docs-lows.

## Issue / plan
Two accepted lows. (1) EVALUATION ORDER: C++ leaves the evaluation order of function arguments unspecified, so a test that calls a stateful RNG two or three times in ONE argument list builds a different value sequence on MSVC and Clang (this already broke a PhysX determinism test once). Found on master: tests/engine/network/n2net/N2NetFuzzTests.cpp around lines 88 and 101, tests/engine/picking/PickAccelerationTests.cpp around line 59. Read both files, rewrite every such call so each RNG draw is its own statement in a fixed order (named locals), keeping what the tests check; then grep the WHOLE tests/ tree (and engine/ for good measure) for any other argument list or operator expression with two or more calls to the same stateful generator (rng(), dist(rng), Next(), Random*, std::uniform_*): fix every one found and list them in the PR body. (2) docs/web-build.html: the Stage 1 paragraph still lists hard-coded per-executable test counts that go stale with every PR: replace them with a description that does not hard-code counts (verify what the web leg actually runs from .github/workflows/tests.yml and the web scripts). Do not change behaviour anywhere else. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^tests/
- ^engine/
- ^docs/

## Acceptance
- No argument list or operator expression in tests/ calls a stateful RNG more than once
- docs/web-build.html Stage 1 has no hard-coded test counts
- All checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: grep yourself for remaining multi-draw expressions; tests still check the same properties; docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs tests-eval-order-docs-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-tests-eval-order-docs-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/tests-eval-order-docs-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-tests-eval-order-docs-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/tests-eval-order-docs-lows/dev.json: {"pr":<number>,"branch":"fix-tests-eval-order-docs-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-tests-eval-order-docs-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
