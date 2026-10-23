# Task lua-ex2-followups: Lua extensions E-X2 follow-ups: total memory cap across extension states, overall reload time limit, logging under the registry mutex, int param min/max validation, and role/play tests that actually run the commands
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: fix-lua-ex2-followups.

## Issue / plan
Follow-up to merged E-X2 (#189, editor-server/src/ExtensionHost.cpp and tests/editor-server/EditorExtensionCommandsTests.cpp; design C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md). The user accepted these as known notes and asked for a follow-up. Read the E-X2 docs in docs/editor.html and docs/scripting-lua.html first and the review notes in C:/Method Notes Obsidian/orchestrator/ledger/lua-ex2-extension-host/review.json. FIX: (1) TOTAL MEMORY CAP: up to 64 files x 64 MiB each can load (4 GiB worst case, no total cap, documented): add a host-wide total cap across all extension states (a named constant, e.g. 256 MiB total, a per-state cap that shrinks or a load refusal for the file that would exceed it with a clear path-stripped reason 'extension memory budget exhausted', deterministic order = the existing sorted discovery order), tests (many small states stay under; a file that would exceed the total fails as DATA and the others still load) and docs. (2) RELOAD TIME LIMIT: a reload has no overall time limit (the command thread waits for files one after another, many seconds with many slow files): add an overall wall-clock budget for a Load/Reload (named constant; files not loaded in time fail as data with 'reload time budget exhausted' and are retried on the next reload, never cached as permanent failures), tests with a blocking hook, docs. (3) Load() logs from _entries after releasing _mutex, breaking the header's promise that the registry is guarded by the mutex: snapshot what is needed under the lock and log from the snapshot (or log under the lock if cheap), keep the header comment true. (4) An int param accepts min/max that are not whole numbers, and with no default takes min as its default so an Int field can get a fractional default: validate that int params have integer min/max/default (a bad shape fails the file as data with a path-stripped reason) and test. (5) The play-host test and the role tests only check the static tables: add tests that really run the commands through ExecuteCommand/the session path: a play host refuses ReloadEditorExtensions, a viewer and an editor may ListEditorExtensions/GetEditorUi but ReloadEditorExtensions is refused with forbidden for them and allowed for admin, a read-only host still lists, extensionsDisabled behaviour when the flag is off. Keep protocol and public behaviour otherwise unchanged (allowProtocol false: STOP and report if a protocol change is needed). Update docs (editor.html, scripting-lua.html, testing.html) and verify every sentence against the CODE. Nothing builds locally; CI (10 checks) is the test; expect fix rounds. Note: another in-flight engine item (MCP0a/0b) edits EditorServer.cpp: keep your changes inside ExtensionHost.* and the extension tests, plus docs.

## Allowed paths (regex, repo-relative)
- ^editor-server/
- ^engine/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$

## Acceptance
- Host-wide total memory cap across extension states with deterministic failure as data, tested
- Overall time limit for load/reload with retry on next reload, tested
- Logging no longer reads the registry outside the mutex; int param shape validation; tests that really execute role/play/flag cases
- All 10 checks green; docs verified against code
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: the caps cannot be bypassed (per-state plus total accounting correct when states fail/abandon/reload), budget-exhausted files are not cached as permanent failures, thread-safety of the logging change, int validation complete (NaN/inf/huge), tests really execute the commands, no change to EditorServer.cpp beyond what is needed, docs true, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs lua-ex2-followups` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref fix-lua-ex2-followups` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/lua-ex2-followups/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-lua-ex2-followups --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/lua-ex2-followups/dev.json: {"pr":<number>,"branch":"fix-lua-ex2-followups","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-lua-ex2-followups` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
