# Task web-loader-lows: Web loader: cancel the first response body on the streaming re-fetch; wrap only real module errors as moduleError (two lows from the stage 3 review)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-web-loader-lows.

## Issue / plan
Two LOW findings from the review of merged PR #174 (web stage 3) in web/n2-loader.js; fix both, add node tests, touch nothing else. Read web/n2-loader.js, the existing node tests for the web player (find them; the CI web job runs them) and docs/web-build.html first. (1) The re-fetch after a failed streaming instantiate never cancels the first body: around n2-loader.js line ~229 the streaming path wraps the response (wrapped.body.locked is always true afterwards, so cancelling the original body is impossible). Fix: set an 'abandoned' flag when the streaming attempt fails and the loader falls back to re-fetching, and call controller.error() (or otherwise cancel the wrapping ReadableStream so the underlying reader is released and the network body is cancelled); no dangling reader, no double count in the progress callback, and the fallback result unchanged. (2) fromBytes ~line 205 wraps EVERY WebAssembly.instantiate rejection as moduleError (the user is told 'different builds, reload'), so an out-of-memory RangeError gets the wrong message. Wrap as moduleError ONLY when the error is a real module error (WebAssembly.CompileError / LinkError, or the loader's isModuleError predicate if one exists: find it) and let other errors (RangeError/OOM, network) propagate with their own correct user-facing message; check how the player/shell displays errors so an OOM gets a sensible message (add a short 'out of memory, close other tabs and reload' style message only if the shell already has a place for per-kind messages; do not redesign error UI). Tests: node tests with mocked fetch/WebAssembly/streams proving (1) the first body is cancelled/errored on re-fetch exactly once and the fallback still loads, (2) CompileError/LinkError -> moduleError, RangeError -> not moduleError; the tests must fail on the old code. Docs: only if docs/web-build.html describes the old behaviour; verify against the code. Nothing builds locally for the engine; the CI web job (node tests) is the test; you may run node locally for the web tests if node is available. Keep the change small.

## Allowed paths (regex, repo-relative)
- ^web/
- ^docs/web-build\.html$
- ^tests/web/
- ^scripts/

## Acceptance
- The first body is cancelled on the streaming re-fetch; fallback unchanged; tested
- Only real module errors become moduleError; OOM RangeError propagates correctly; tested
- All 10 checks green

## Notes
Reviewer: stream cancellation correctness (no double cancel, no unhandled rejection), error classification, tests are meaningful (fail on the old code), nothing else changed.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-loader-lows` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-web-loader-lows` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-loader-lows/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-web-loader-lows --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-loader-lows/dev.json: {"pr":<number>,"branch":"fix-web-loader-lows","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-web-loader-lows` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
