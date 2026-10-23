# Task docs-web-stage5-lows: Docs: correct the three accuracy lows left in docs/web-build.html after web stage 5 (test counts, cold-cache minutes, stage-6 threading statement)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: docs-web-stage5-lows.

## Issue / plan
Docs-only item. Three known inaccuracies from the review of merged PR #184 (web stage 5) in docs/web-build.html, each to be verified against the CODE, CI configuration and measured CI logs before editing: (1) the 'The PhysX tests under node' section states engine_tests counts (1504 tests: 1490 passed, 14 skipped) that do not match the CI run at that head (1529 passed, 14 skipped, 1543 in all): do NOT hard-code counts that go stale: rewrite so the claim is about what is true and stable (e.g. which PhysX tests run under node and which are skipped and why) and, if counts are kept, say they are examples as of PR #184 and verify them from a current web CI log (use the wrapper: bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/actions/... for the latest master web job; be gentle with the API, no log downloads beyond one job); (2) the design-decision callout says the cold-cache PhysX source build takes 'about four minutes of one job' but the CI bullet above it measures 102 s of PhysX build, 122 s of configure and 'about 2 minutes more on a cold cache': read both, check the workflow's cache key and the CI timings, and make the two statements agree and be true; (3) the page says a threaded stage-6 artifact 'would not define the macro' (N2ENGINE_SINGLE_THREADED), as if that alone would bring the PhysX workers back: in fact the web PhysX libraries are built WITHOUT -pthread and neither the .pin stamp nor the CI cache key records threading, so a threaded artifact needs a separately built, separately keyed threaded PhysX: say that (verify in cmake/PhysXWeb.cmake, cmake/PhysXWebPin.cmake, scripts/web-build-physx.sh and the workflow). Keep edits minimal, re-read the HTML (well-formed), no other changes. Also check docs/testing.html and docs/physics.html for the same stale counts or claims and fix only verifiably wrong text.

## Allowed paths (regex, repo-relative)
- ^docs/

## Acceptance
- The three inaccuracies corrected and verified against code/CI; no stale hard-coded counts
- HTML well-formed; docs-only; all checks green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: every changed sentence is true against the code and the CI configuration/logs, no stale numbers introduced, HTML well-formed, nothing else changed, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs docs-web-stage5-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref docs-web-stage5-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/docs-web-stage5-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head docs-web-stage5-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/docs-web-stage5-lows/dev.json: {"pr":<number>,"branch":"docs-web-stage5-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/docs-web-stage5-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
