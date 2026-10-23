# Task fe-f2-prefs-keymap: Electron UI F2: editor prefs store (user and project scopes) and a keymap registry with scoped chords and one dispatcher, migrating the scattered shortcut handlers; plus the F1 follow-up (stable panel-stack ids)
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-fe-f2-prefs-keymap.

## Issue / plan
Electron UI roadmap C:/Method Notes Obsidian/orchestrator/proposals/electron-ui-roadmap.md: the user accepted ALL U1-U21 on 2026-10-20. F1 (panel registry, PR #34) is merged. Read the roadmap section 3 (Foundation F2: prefs store and keymap), decisions U2, U3, U4 and the shortcut notes in sections 1 and 16, the frontend README and the current shortcut sites FIRST: src/renderer/editor.tsx (Ctrl+S and Undo/Redo ~lines 350-395), hierarchy-panel.tsx (F2/Del/Ctrl+D/arrows), viewport-panel.tsx and viewport-input.ts (F/Esc/fly keys, gizmo), console/assets panels, ui.tsx (modalOpen, isTextEntry helpers), the F1 layout code (src/renderer/layout/*) and how F1 persists layout (the localStorage-with-try/catch layer the roadmap says F2 adopts). THIS ITEM IS F2 + ONE F1 LOW: (1) PREFS STORE (src/renderer/prefs.ts or similar, framework-free, unit tested): typed keys with defaults and validators, two scopes (user: global per machine; project: keyed by the project's projectId as F1 does), subscribe/notify, debounced writes, every storage access in try/catch, corrupt or wrong-typed values fall back to the default and are dropped, a version field, no secrets ever stored (assert in a test that a token-looking key is refused or never present); migrate F1's layout persistence to sit on this store WITHOUT changing the stored layout format or losing existing saved layouts (read the old keys, write through the store: keep reading the old key as a fallback for one release and document); (2) KEYMAP (src/renderer/keymap.ts): a registry of NAMED ACTIONS (edit.undo, edit.redo, file.save, hierarchy.rename/delete/duplicate, viewport.frame, viewport.cancel, gizmo.translate etc. only where the action exists today: do NOT add new features; reserve the names the roadmap lists for FE3/FE4 as comments, not code) with DEFAULT chords, a scope (global, viewport, hierarchy, assets, console, text), chord parsing/formatting (Ctrl/Cmd mapping per platform: Cmd on macOS where the app already does so), conflict detection, and ONE dispatcher installed at the document level that respects isTextEntry (no shortcut fires while typing in an input/textarea/contenteditable unless the action is marked allowInText) and modalOpen() and the right scope (focus-based: the focused panel decides viewport/hierarchy scope), with the viewport's hold-right-button fly-key priority preserved exactly as today; MIGRATE every existing shortcut site to register actions with the keymap instead of per-component keydown listeners with IDENTICAL behaviour for the default chords (a regression test per migrated shortcut: simulate the key event in the DOM test harness, assert the same effect); user rebinding storage (user-scope pref 'keymap.overrides': a map action -> chords) is supported by the registry and tested (override, reset, conflict reported) but there is NO rebinding UI yet; (3) a tiny read-only 'Keyboard shortcuts' list in the Window or Help menu showing action names and current chords (so shortcuts are discoverable) with keyboard accessible markup, no editing; (4) F1 LOW: the layout view keys stacks by index (key={i}); closing or reopening a panel shifts later stacks' keys and can remount them: give each stack a stable id in the layout model (generated deterministically, persisted, migrated for old layouts that lack ids) and key by it; add a DOM test that closing/reopening a panel in a region does not remount the other stacks (a mount counter on a stand-in panel); (5) TESTS: node unit tests for prefs (scopes, corrupt, defaults, debounce, subscribe), keymap (parsing, platform mapping, scope, text-entry suppression, modal suppression, conflicts, overrides), DOM tests for each migrated shortcut and the shortcuts list; keep ALL existing tests green; run typecheck and the whole test suite locally before pushing (npm run typecheck; npm test). (6) README: the shortcuts/prefs section (match style, verify against code). Rules: requiresDocs false (README only), no engine/protocol change, no token in logs/storage, do not touch files of the in-flight items beyond what is needed: frontend-protocol-1-14 touches src/protocol and engine-ref.txt only; the MCP1 item adds a new top-level mcp/ directory.

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/
- ^tsconfig[^/]*\.json$

## Acceptance
- Prefs store with user and project scopes, validation and safe storage; F1 layout persistence runs on it with old saved layouts still loading
- Keymap registry with named actions, scoped chords, one dispatcher that respects text entry, modals and the viewport fly-key priority; all existing shortcuts migrated with unchanged default behaviour and a regression test each; override storage tested, no rebinding UI
- Read-only shortcuts list; stable stack ids so closing/reopening a panel does not remount others
- Unit and DOM tests; typecheck, build, tests and frontend CI green; README updated

## Notes
Reviewer: no shortcut regresses (compare every migrated site: same chord, same scope, same text-entry behaviour, viewport fly keys with the right button held), dispatcher cannot double-fire or leak listeners, storage failures never throw, old layout keys still load, no secret stored, stable ids correct for migrated layouts, tests meaningful, mediums count. Unverified: real Electron focus behaviour across panels.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-f2-prefs-keymap` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-f2-prefs-keymap/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-fe-f2-prefs-keymap --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-f2-prefs-keymap/dev.json: {"pr":<number>,"branch":"feat-fe-f2-prefs-keymap","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-fe-f2-prefs-keymap` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
