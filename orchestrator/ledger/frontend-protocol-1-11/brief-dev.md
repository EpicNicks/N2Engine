# Task frontend-protocol-1-11: Sync to protocol 1.11.0: read-only host badge, ErrorCoded denials as toasts, writes disabled
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-protocol-1-11-readonly.

## Issue / plan
Engine master 2705888 (PR #126) shipped protocol 1.11.0. Scope: (1) bump engine-ref.txt to the engine commit 2705888a93a9cf06c3dc685d74933c6a7a06f135 (or the then-current master if the protocol is unchanged since) and run `npm run sync-protocol` (default ../N2Engine; the engine checkout at C:/Method Notes Obsidian/N2Engine may be used READ-ONLY for this) so src/protocol/protocol.generated.ts and test-vectors.json are regenerated, never hand-edited; keep their line endings as the script does. (2) engine-client: Hello now sets the new trailing flag `errorCodes` so the host answers a refused command with the new response ErrorCoded (0x1C: message + code readOnly|forbidden) instead of a plain Error; expose the code on the thrown error (a typed EngineError with `code`, keep existing Error handling and messages working for 1.10 hosts which never send ErrorCoded and have no ServerInfo.readOnly: the trailing field decodes to its default false). (3) Read ServerInfo.readOnly into the app state (store) and show a clear read-only badge in the editor chrome; disable (not hide) every write affordance while read-only: hierarchy create/delete/rename/reparent/duplicate, inspector edits, asset import/delete/move/rename, scene save/new/open-write, play, undo/redo, drag and drop; find the write call sites through the store/edit-controller rather than per-widget where possible so a new write cannot be forgotten (a single `canWrite` guard). (4) A denial that still reaches the client (readOnly or forbidden code) shows a toast with the host's message and does NOT leave the UI in a half-applied state (revert optimistic edits if there are any). (5) Tests: codec/golden-vector tests pass after the sync; unit tests for the ErrorCoded decode and the error mapping; DOM tests (the harness exists, see src/test-dom and the README testing section) for the badge and disabled controls and the denial toast; a 1.10 host (no readOnly, plain Error) still works. (6) README: protocol version, read-only mode behaviour. Run npm run typecheck, npm run build and npm test locally; the repo also has CI, so open the PR and wait for its checks. Do not change the engine repo. Do not change main-process connection security code (TLS pinning, tokens). If a write path cannot be guarded without a design decision, stop and report needsUser.

## Allowed paths (regex, repo-relative)
- ^engine-ref.txt$
- ^package.json$
- ^package-lock.json$
- ^README.md$
- ^src/
- ^scripts/
- ^.github/

## Acceptance
- sync-protocol produces no diff against engine-ref.txt (CI check passes)
- ServerInfo.readOnly shown as a badge; every write affordance disabled while read-only
- ErrorCoded decoded and shown as a toast; 1.10-style plain Error still works
- typecheck, build, tests and frontend CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: check the write guard is central, no write path is missed (grep the store/edit-controller/ipc call sites), the toast does not leak tokens, generated files are untouched by hand, CRLF preserved. Unverified: the Electron UI against a live --read-only host (user hardware checklist).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-protocol-1-11` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-11/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-protocol-1-11-readonly --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-11/dev.json: {"pr":<number>,"branch":"feat-protocol-1-11-readonly","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-protocol-1-11-readonly` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
