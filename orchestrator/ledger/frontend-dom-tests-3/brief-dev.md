# Task frontend-dom-tests-3: DOM tests PR 3: assets panel and hierarchy panel flows
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-dom-tests-assets-hierarchy.

## Issue / plan
Approved plan: C:/Method Notes Obsidian/orchestrator/proposals/frontend-dom-tests.md (section 4 item 4, section 5 jsdom gaps, section 6 PR 3; read it first). Harness from PRs #27/#29 exists (src/test-dom/setup.ts, helpers.tsx with renderWithApp/renderWithStore, src/test/fake-store-api.ts); read them and the existing read-only DOM test (src/test-dom/read-only.test.tsx) first, and reuse their patterns. Scope: DOM tests for src/renderer/assets-panel.tsx and src/renderer/hierarchy-panel.tsx over AssetsState/HierarchyState (or the real store) with a fake engine: assets: context menu items per asset kind and which are enabled, rename prompt flow (type new name, confirm, the right state/engine call with the right args; cancel and invalid names), delete confirmation, tree expand/collapse; hierarchy: tree rendering from fake entities, expand/collapse, selection, rename and create/delete flows and their calls, disabled state when the host is read-only (writeguard). Assert calls and resulting DOM, not just rendering. Drag and drop: jsdom has no DataTransfer; test drop handlers via fireEvent.drop with a hand-made dataTransfer object only if simple, else skip and say so in the README note. Out of scope: viewport panel (canvas/ResizeObserver), layout/positioning. Extend the shared fake additively only if needed. README testing note: what these tests cover. Run npm run typecheck, build and npm test locally; open a PR and wait for repo CI. Do NOT change production code under src/main, src/preload or src/renderer, except exporting a symbol purely for testing if unavoidable (report it as a deviation); if a test reveals a real bug stop and report needsUser. No new dependencies.

## Allowed paths (regex, repo-relative)
- ^src/test-dom/
- ^src/test/
- ^README.md$

## Acceptance
- typecheck, build, npm test and frontend CI green
- Assets and hierarchy flows asserted (calls and DOM)
- No production code changed

## Notes
Reviewer: tests must assert behaviour, no production file changed, no flaky timing.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-dom-tests-3` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-dom-tests-3/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-dom-tests-assets-hierarchy --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-dom-tests-3/dev.json: {"pr":<number>,"branch":"feat-dom-tests-assets-hierarchy","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-dom-tests-assets-hierarchy` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
