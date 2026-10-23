# Task frontend-protocol-1-14: Frontend: sync to protocol 1.14.0 (Lua editor extensions commands, event, capability and error codes): typed client support only, no UI
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-protocol-1-14-extensions.

## Issue / plan
Engine PR #189 (E-X2, merged) shipped protocol 1.14.0 (additive minor): commands ListEditorExtensions 0x80 (Read), GetEditorUi 0x81 (Read), ReloadEditorExtensions 0x84 (Admin) (0x82/0x83 reserved for later), an EditorEvent kind extensionsChanged {uiRevision} (EditorEvent gained an optional uiRevision), new errorCodes extensionsDisabled / extensionFailed / extensionFaulted, and ServerInfo.capabilities containing "editorExtensions" when the host supports them. Read the engine repo (C:/Method Notes Obsidian/N2Engine, READ-ONLY) editor-server/protocol/protocol.json, docs/editor.html and docs/logging-and-editor.html for the exact semantics and the design C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md section 7 (7.1 commands, 7.2 frontend) first. SCOPE (typed client support ONLY: the Tools menu, form dialog, trust prompt and admin dialog are the later E-X4 item; do NOT build UI): (1) bump engine-ref.txt to the current engine master commit and run `npm run sync-protocol` so src/protocol/protocol.generated.ts and test-vectors.json are regenerated, NEVER hand-edited (the CI 'Generated protocol matches the pinned engine' check must stay green); (2) engine-client: typed wrappers listEditorExtensions(), getEditorUi(), reloadEditorExtensions() returning typed results (parse the JSON payloads defensively: unknown fields ignored, missing fields defaulted, bad shapes become a typed error, never a throw into the UI), expose serverInfo capabilities (a helper hasCapability('editorExtensions')), map the three new error codes to typed errors distinct from forbidden/readOnly/busy (follow how 1.11-1.13 coded errors such as forbidden/readOnly/busy are mapped), handle the extensionsChanged event and the optional uiRevision in the event model (store a uiRevision signal other code can subscribe to; no UI); (3) old hosts: against a 1.13 host (no capability, commands unknown) nothing changes: the helpers report 'unsupported' without sending the commands; (4) tests: node unit tests with the existing fake host/fixtures for the typed wrappers, the error-code mapping, event parsing, old-host behaviour and defensive parsing of malformed payloads; keep every existing test green; typecheck, build and tests run locally (npm run typecheck; npm test): run them before pushing; (5) README: the Protocol section (version 1.14.0 and what the client supports) and a short note that extension UI is not built yet; verify against the code. Rules: requiresDocs false (README only), no token in logs/storage, no UI changes (the in-flight F1 panel-registry item edits editor.tsx/panels.tsx/ui.tsx: stay out of those files).

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/

## Acceptance
- sync-protocol produces no diff against engine-ref.txt (CI check passes)
- Typed client wrappers for the three commands, error-code mapping, extensionsChanged event and capability helper; old hosts unaffected
- Defensive payload parsing tested; no UI added; typecheck, build, tests and frontend CI green

## Notes
Reviewer: no hand edits to generated files (sync-protocol output only), old-host path sends nothing, defensive parsing cannot throw into the UI, error codes mapped distinctly, no token persisted/logged, no UI files touched (editor.tsx/panels.tsx/ui.tsx), tests meaningful. Unverified: a live 1.14 host.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-protocol-1-14` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-14/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-protocol-1-14-extensions --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-14/dev.json: {"pr":<number>,"branch":"feat-protocol-1-14-extensions","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-protocol-1-14-extensions` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
