# Task fe-fe8-history-panel: Electron UI FE8: history panel over GetHistory with visible, jumpable undo steps (sequential Undo/Redo) and edit-group awareness
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-fe8-history-panel.

## Issue / plan
Roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md section 7 (Candidate 4: history panel), decision U19 (history jump = sequential Undo/Redo from the client; a host UndoTo only if jumps prove slow). Read edit-controller.ts, edit-groups.ts, edit-actions.ts, the Edit menu code that already uses GetHistory, panels.tsx (register the new panel through the F1 registry as a closable panel in the default bottom/side group the roadmap names), the README undo/edit-groups section and the engine docs for GetHistory (/engine repo docs/editor.html: READ-ONLY) FIRST. SCOPE: a History panel listing the host's undo stack and redo stack (labels, revision numbers, which is current, the saved revision marker, group boundaries as the host reports them), live-updated on the history events the host sends (historyChanged: find how the client already reacts), clicking an entry undoes or redoes sequentially to that point as ONE visible operation with a progress state (cancel supported between steps), disabled when the host is read-only/viewer or when a client edit group is open or when a step fails (show the error; stop; re-read the history), keyboard accessible list (arrow keys, Enter to jump, Escape), large histories virtualised or capped as the host caps them, tests: pure logic (diff of history lists, jump planning, failure handling) and DOM tests with a fake edit backend (jump forward/back, failure mid-jump, read-only, concurrent change by the host, empty), README updated. Frontend rules: requiresDocs false (README only); run npm run typecheck and npm test locally before every push; no token in logs/storage; keep changes inside the files your lane owns (other frontend lanes are in flight at the same time: keymap action registrations and README additions will conflict mechanically, keep them as small separate hunks); no engine/protocol change unless this item says so. Read the roadmap section named in this item, decisions U1-U21 (all ACCEPTED by the user on 2026-10-20), the frontend README and the code you touch FIRST; F1 (panel registry/layout) and F2 (prefs store and keymap) are merged: use them (register panels through the registry, shortcuts through the keymap, preferences through the prefs store).

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- History panel registered through the F1 registry, live-updated, shows current/saved markers
- Sequential jump with progress, cancel, failure handling, read-only/group-open disabled states
- Unit and DOM tests; frontend CI green; README updated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: jump never leaves the project in an unexpected state (failure mid-jump re-syncs), concurrency with host events, disabled states, keyboard access, no listener leaks, tests meaningful, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-fe8-history-panel` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-fe8-history-panel/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-fe8-history-panel --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-fe8-history-panel/dev.json: {"pr":<number>,"branch":"feat-fe-fe8-history-panel","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-fe8-history-panel` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
