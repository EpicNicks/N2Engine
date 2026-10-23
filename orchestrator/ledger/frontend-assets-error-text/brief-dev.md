# Task frontend-assets-error-text: assets-state: strip the error-code prefix from host errors and report coded denials to the banner
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: fix-assets-state-error-text.

## Issue / plan
Follow-up to the reviewed low finding on merged frontend PR #28. In src/renderer/assets-state.ts `messageOf` (around line 94) reads the raw error message, so a coded denial (the code travels as a U+2063-delimited message prefix, see errorCodeOf/errorTextOf in src/shared/api.ts and how src/renderer/inspector-state.ts handles it) would show the code glued to the host text; the catch blocks that save import settings and save a text file (around lines 346 and 475) set only the inline error. Fix: use errorTextOf in messageOf, and for a coded denial (readOnly/forbidden) in those catch blocks also call the state's error callback (store.reportError: banner + deniedAsReadOnly), keeping the inline error; plain failures stay inline only. Mirror what inspector-state.ts already does. Add unit/DOM tests (existing harness in src/test-dom and src/test) for both: coded denial shows clean text and reaches the banner callback; plain error unchanged. Run npm run typecheck, build and npm test; open a PR and wait for repo CI. No new dependencies; no other behaviour change.

## Allowed paths (regex, repo-relative)
- ^src/renderer/assets-state.ts$
- ^src/renderer/assets-panel.tsx$
- ^src/renderer/store.ts$
- ^src/test/
- ^src/test-dom/
- ^README.md$

## Acceptance
- typecheck, build, npm test and frontend CI green
- coded denial in assets saves shows clean text, reaches the banner, and latches read-only for readOnly
- plain errors unchanged

## Notes
Small follow-up; reviewer checks it mirrors inspector-state.ts and tests assert behaviour.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-assets-error-text` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-assets-error-text/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head fix-assets-state-error-text --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-assets-error-text/dev.json: {"pr":<number>,"branch":"fix-assets-state-error-text","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/fix-assets-state-error-text` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
