# Task fe-f1-panel-registry: Electron UI F1: panel registry with tab stacks in the existing regions, Window menu, and per-project persisted layout with reset (foundation for the roadmap features, E-X4 and the Network panel)
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-f1-panel-registry.

## Issue / plan
Electron UI roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md: the user accepted ALL recommendations U1-U21 on 2026-10-20. Read sections 0-3 (current state, F1, F2) and the decisions U1, U2, U21 FIRST, then the frontend README (Architecture, panels) and the renderer code it names: src/renderer/editor.tsx, panels.tsx, ui.tsx, store.ts, the CSS, the existing splitter handling, and every panel mount (hierarchy-panel.tsx, inspector-panel.tsx, assets-panel.tsx, console-panel.tsx, viewport-panel.tsx, welcome.tsx). THIS ITEM IS F1 ONLY (F2: prefs store + keymap is the NEXT item: do not build the keymap; but build the small persistence layer F1 needs behind an interface F2 can adopt). SCOPE per U1/U2: (1) a lightweight PANEL REGISTRY (src/renderer/panel-registry.ts or similar, framework-free, unit-testable): panels register {id, title, icon?, component, defaultRegion, closable, singleton, minSize?}; four existing regions (keep the current layout as the DEFAULT arrangement, pixel-comparable in structure); each region holds a TAB STACK (one or more panels, active tab); panels can be moved between regions (drag a tab to another region's tab bar, plus a 'Move to' menu for keyboard users), closed (if closable) and reopened from a new Window menu, which also has 'Reset layout'; NO docking library and NO floating windows (U1); (2) a layout model: serialisable {version, regions: {id: {tabs: [panelId], active, size}}, splitter sizes} with validation and migration (unknown panel ids dropped, missing panels appended to their default region, corrupt or newer-version data falls back to the default without throwing); (3) PERSISTENCE per project (U2): renderer localStorage keyed by the project's projectId (find how the app identifies a project: projectId from GetProjectInfo or the host launcher's recent-projects store) with try/catch around every storage access (storage may be unavailable), a user-level default layout key, debounced saves, per-machine only; Window > Reset layout clears the project's layout; (4) migrate every existing panel to register through the registry with NO behaviour change to the panels themselves; the splitter sizes now persist (today they are session-only); the viewport panel keeps its canvas lifecycle when tabs switch (hidden tab: keep mounted or pause rendering: decide per the roadmap section and record it: the viewport must not leak frame requests while hidden); (5) accessibility: tab bar roles (tablist/tab/tabpanel), keyboard navigation (arrows, Home/End), focus visible; (6) TESTS: node unit tests for the layout model (validate/migrate/corrupt/newer version, move/close/reopen/reset reducers), DOM tests (src/test-dom) for tab switching, move between regions, close/reopen via Window menu, reset, restore from storage, storage-unavailable path, viewport frame requests stop while hidden; keep ALL existing tests green; (7) README: the layout/Window menu section (match the README's style and verify against the code). Frontend CI runs typecheck, build, node and DOM tests: you CAN run them locally (npm run typecheck; npm test): do, before pushing. Run the app-level checks you can; list what needs the user (the feel of tab drag, real Electron window) as unverified. Frontend rules: requiresDocs false (README only), no engine/protocol change, no token in logs/storage, never persist secrets in the layout.

## Allowed paths (regex, repo-relative)
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- Panel registry with tab stacks in the four regions, Window menu (open/close/reset) and keyboard-accessible tab bars; default arrangement unchanged
- Layout persisted per project (localStorage, try/catch) with validation/migration and a user default; splitter sizes persist; reset works
- All existing panels registered with no behaviour change; viewport does not leak frame requests while hidden
- Unit and DOM tests; typecheck, build, tests and frontend CI green; README updated

## Notes
Reviewer: no behaviour change to existing panels (diff mounts), layout validation never throws on corrupt storage, storage access always guarded, timers/listeners cleaned up on unmount, viewport hidden-tab handling leaks nothing, keyboard accessibility of the tab bar, no secret ever stored, tests meaningful (fail on regressions), README true, mediums count. Unverified: drag feel in a real Electron window.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-f1-panel-registry` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-f1-panel-registry/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-f1-panel-registry --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-f1-panel-registry/dev.json: {"pr":<number>,"branch":"feat-fe-f1-panel-registry","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-f1-panel-registry` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
