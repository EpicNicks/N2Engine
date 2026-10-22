# Task frontend-protocol-1-15: Frontend: sync to protocol 1.15.0 (InvokeMenuItem 0x82, extensionOutput event, staleUiRevision and editGroupOpen error codes): typed client support only, no UI
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-protocol-1-15-invoke.

## Issue / plan
Engine PR #202 (E-X3, merged) shipped protocol 1.15.0 (additive minor on 1.14.0): command InvokeMenuItem 0x82 (Write class; request itemId string, selection (entity UUID array, bounded), params JSON, uiRevision u32; response ExtensionResult JSON {message?, select?[], warnings?[]} plus the edit result (revision, savedRevision, canUndo, canRedo, label)), the EditorEvent kind extensionOutput {extension, level, message} (Admin-only events), new errorCodes staleUiRevision and editGroupOpen (plus the 1.14.0 extensionsDisabled/extensionFailed/extensionFaulted), 0x83 still reserved. Read the engine repo (C:/Method Notes Obsidian/N2Engine, READ-ONLY): editor-server/protocol/protocol.json, docs/editor.html (the 'running a menu item' section), docs/logging-and-editor.html (1.15.0 protocol section), docs/scripting-lua.html, and the merged frontend 1.14.0 client work in src/protocol (engine-client.ts typed wrappers for ListEditorExtensions/GetEditorUi/ReloadEditorExtensions, error-code mapping, extensionsChanged event, capability helper) FIRST. SCOPE (typed client support ONLY; the Tools menu, FormDialog, trust prompt, launcher flag and the admin Extensions dialog are the NEXT item E-X4: do NOT build UI): (1) bump engine-ref.txt to the current engine master commit and run `npm run sync-protocol` so src/protocol/protocol.generated.ts and test-vectors.json are regenerated, NEVER hand-edited (the CI 'Generated protocol matches the pinned engine' check must stay green); fix tests asserting the protocol version (1.14.0 -> 1.15.0: engine-client-busy.test.ts, mcp/test/tools-read.test.ts and any others: grep for '1.14.0'); (2) engine-client: typed wrapper invokeMenuItem(itemId, selection, params, uiRevision) returning a typed ExtensionResult (parse defensively: unknown fields ignored, missing defaulted, bad shape => typed error, never a throw into the UI), typed errors for staleUiRevision and editGroupOpen distinct from forbidden/readOnly/busy/extension* (follow how the 1.14.0 codes are mapped), the extensionOutput event in the event model (an observable log store another item can subscribe to: bounded ring, level and extension fields, strings treated as UNTRUSTED text: never HTML), a capability gate (old hosts without 'editorExtensions' or a 1.14 host: invokeMenuItem reports 'unsupported' without sending); (3) tests: node unit tests with the fake host fixtures for the wrapper, both new error codes, the event, old-host behaviour and defensive parsing; keep every existing test green; typecheck, build and tests run locally before pushing; (4) README: the Protocol section (1.15.0) and a short note that the Tools menu UI is not built yet; verify against the code. Rules: requiresDocs false; no token in logs/storage; NEVER run taskkill; no UI changes; README additions as separate small hunks (other frontend PRs are in flight and README collides mechanically); your FINAL message must be a real report (PR number, head sha, deviations).

## Allowed paths (regex, repo-relative)
- ^engine-ref\.txt$
- ^package\.json$
- ^package-lock\.json$
- ^README\.md$
- ^src/
- ^scripts/
- ^mcp/

## Acceptance
- sync-protocol produces no diff against engine-ref.txt (CI passes); version assertions updated
- Typed invokeMenuItem wrapper, the two new error codes, extensionOutput event store and capability gate; old hosts unaffected
- Defensive parsing tested; no UI added; typecheck, build, tests and frontend CI green

## Notes
Reviewer: no hand edits to generated files, old-host path sends nothing, defensive parsing cannot throw into the UI, extensionOutput text never reaches HTML unescaped, error codes mapped distinctly, no UI files touched, tests meaningful, mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs frontend-protocol-1-15` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-15/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-protocol-1-15-invoke --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/frontend-protocol-1-15/dev.json: {"pr":<number>,"branch":"feat-protocol-1-15-invoke","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-protocol-1-15-invoke` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
