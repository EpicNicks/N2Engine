# Task web-g4-e6-lows: Follow-ups: G4 async-readback coverage, WebGpuFactory comment, isHeadless docs row, web-build size table; E-X6 code lows
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-web-g4-e6-lows.

## Issue / plan
Follow-ups of merged G4 (#208) and E-X6 (#215); the user accepted these LOW findings as a follow-up. Verify each against the CODE on master first and say what you did not change and why. G4/web: (1) the async readback path of the WebGPU renderer compiles but no test runs it (engine/src/renderer/.../WebGpuRenderer.cpp ~line 428): add the best coverage achievable without a browser GPU (a test in the web node tests or a pure-logic test of the readback state machine/ordering, e.g. factor the pure part out) or, if truly impossible, a clearly marked TODO in docs 'Not verified' naming G5/G6; (2) engine/include/.../WebGpuFactory.hpp comment says the factory is declared only where compiled in but the header declares it unconditionally: make comment and code agree (prefer fixing the comment unless the conditional declaration is trivially right); (3) docs/application.html isHeadless row (~line 128) and the Headless mode intro name only SOFTWARE and WEBGPU as the backends without a window: NONE also has no window: say so; (4) docs/web-build.html #size-report: the Limit column still shows the pre-Jolt limits (a J3 leftover) while web/budgets.json holds the raised ones: make the table match web/budgets.json exactly (read the file; do not invent numbers) and check the sentences around it. E-X6: (5) OnBeforePlay runs and broadcasts its warnings before WritePlaySnapshot validates the request, with the unvalidated client scene path in the message: move the hook after validation or sanitise the path in the message; test; (6) an extension's own asset write never reaches any OnAssetsChanged hook because NoteAssetWritten refreshes the index so no later poll sees a change: this was a DECLARED deviation, so only make the docs state it plainly and precisely if they do not already; (7) the watcher never retries a file that failed on an id clash when the owning file changes, and the untouched extension always wins: retry the failed file when the clash owner changes or is removed; test. Also one tiny frontend-unrelated engine item: none. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^editor-server/
- ^web/
- ^tests/
- ^docs/
- ^\.github/

## Acceptance
- Readback covered or honestly documented; WebGpuFactory comment true
- Docs rows (isHeadless, size table) true against code and budgets.json
- OnBeforePlay hook runs after validation; watcher retries an id-clash file; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: size table equals web/budgets.json exactly, hook ordering test, retry cannot loop or reload an unchanged file repeatedly, docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-g4-e6-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-web-g4-e6-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-g4-e6-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-web-g4-e6-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-g4-e6-lows/dev.json: {"pr":<number>,"branch":"fix-web-g4-e6-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-web-g4-e6-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
