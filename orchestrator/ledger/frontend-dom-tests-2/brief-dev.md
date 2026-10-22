# Task frontend-dom-tests-2: DOM tests PR 2: store-driven welcome behaviour over a shared fakeStoreApi
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-dom-tests-welcome-store.

## Issue / plan
Approved plan: C:/Method Notes Obsidian/orchestrator/proposals/frontend-dom-tests.md (section 4 item 3 and section 6 PR 2; read it first). The harness (src/test-dom/setup.ts, helpers.tsx, renderWithApp) already exists from PR #27 and the frontend now also has write-guard/read-only code (PR #28), so re-read the current src/test-dom and src/test/store.test.ts first. Scope: (1) extract `fakeStoreApi()` from src/test/store.test.ts into a shared helper under src/test/ (e.g. src/test/fake-store-api.ts) as a PURE MOVE with identical behaviour; store.test.ts imports it; extend it only additively if a DOM test needs a method. (2) Add a renderWithStore (or an option of renderWithApp) in src/test-dom/helpers.tsx that builds a REAL EditorStore over fakeStoreApi() so signals drive the DOM. (3) Tests in src/test-dom for the Welcome screen (src/renderer/welcome.tsx) driven by the real store: the Recent Projects list renders the recent entries and its Remove button removes one (store/api call asserted, list updates); Open Project is disabled when hostLocation.problem is set and enabled otherwise; recent remotes behave the same way if the component shows them; empty states. Assert real behaviour (calls and resulting DOM), not just that things render. (4) README testing note: mention the shared fake and the store-driven helper. Run npm run typecheck, npm run build and npm test locally (all must pass); open a PR, wait for the repo CI. Do NOT change production code under src/main, src/preload or src/renderer unless a test reveals a real bug: then stop and report needsUser. No new dependencies.

## Allowed paths (regex, repo-relative)
- ^src/test-dom/
- ^src/test/
- ^README.md$
- ^package.json$

## Acceptance
- typecheck, build, npm test and frontend CI green
- fakeStoreApi shared (pure move); store tests unchanged in behaviour
- Welcome store-driven tests assert recent list/remove and Open Project disabled state
- No production code changed

## Notes
Frontend repo has CI now. Reviewer: confirm the extraction is a pure move, tests assert behaviour not just render, no production file changed.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-dom-tests-2` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-dom-tests-2/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-dom-tests-welcome-store --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-dom-tests-2/dev.json: {"pr":<number>,"branch":"feat-dom-tests-welcome-store","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-dom-tests-welcome-store` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
