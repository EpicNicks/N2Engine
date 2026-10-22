# Task fe-ex5-inspector-sections: Lua extensions E-X5 frontend: sync to protocol 1.16.0 and inspector sections with InvokeInspectorAction
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-ex5-inspector-sections.

## Issue / plan
Engine PR #209 (E-X5, merged) shipped protocol 1.16.0 (additive minor on 1.15.0): command InvokeInspectorAction 0x83 (Write class; componentId, actionId, params JSON, uiRevision; response ExtensionResult plus the edit result) and the inspector sections carried by GetEditorUi. Design: C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md (D1-D12 approved; read the Inspector sections paragraph near line 314, the command table near line 291, the role table near 234). Read FIRST in the engine repo (READ-ONLY): editor-server/protocol/protocol.json, docs/editor.html, docs/logging-and-editor.html (1.16.0 section), docs/scripting-lua.html, and the merged frontend E-X3/E-X4 work (src/protocol engine-client typed wrappers, the Tools menu, FormDialog, trust prompt, extension console) so the new code follows it. SCOPE: (1) bump engine-ref.txt to the current engine master commit and run npm run sync-protocol (generated files never hand-edited; CI Generated-protocol check stays green); update tests asserting the protocol version (grep 1.15.0, including mcp/ tests); (2) engine-client: typed invokeInspectorAction(componentId, actionId, params, uiRevision) returning the typed ExtensionResult (defensive parsing, typed errors never thrown into the UI, same staleUiRevision/editGroupOpen/forbidden handling and capability gate as invokeMenuItem, reports unsupported on old hosts without sending); (3) inspector UI: under a component whose type matches a section's componentType in inspector-panel.tsx render the section label, read-only fields (static or labelled values) and buttons; a button opens its FormDialog (when it declares params) and sends InvokeInspectorAction; hidden/disabled for roles that lack Write; results (message/select/warnings) handled exactly like menu-item results; ALL extension strings are UNTRUSTED text rendered as text, never HTML; refresh on the extensionsChanged event and on stale ui revision; (4) tests: node unit tests with fake host fixtures for the wrapper, errors, gate and defensive parsing; DOM tests for section rendering, button flow, role gating, stale revision; keep every existing test green; (5) README section. Do not touch the engine repo. Frontend repo C:/Method Notes Obsidian/N2Engine-Electron-Frontend (EpicNicks/N2Engine-Electron-Frontend); the engine repo C:/Method Notes Obsidian/N2Engine is READ-ONLY reference. Tests run LOCALLY: npm run typecheck, npm run build and npm test must pass before you push; CI has 3 checks. NEVER run taskkill or kill other processes; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via bash "C:/Method Notes Obsidian/gh-epicnicks.sh" run inside the repo; README additions as ONE small self-contained section (other frontend PRs append too); before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine-ref.txt$
- ^package.json$
- ^package-lock.json$
- ^README.md$
- ^src/
- ^scripts/
- ^mcp/

## Acceptance
- sync-protocol produces no diff against engine-ref.txt; version assertions updated to 1.16.0
- Typed invokeInspectorAction with errors and capability gate; old hosts unaffected
- Inspector sections render read-only fields and buttons; button flow with form, role gating, stale revision, untrusted text as text
- typecheck, build, tests and frontend CI green

## Notes
Reviewer (security/protocol): extension text never rendered as HTML, role gating matches the table, no UI action reachable without Write, defensive parsing, generated files untouched by hand.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs fe-ex5-inspector-sections` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-inspector-sections/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-ex5-inspector-sections --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/fe-ex5-inspector-sections/dev.json: {"pr":<number>,"branch":"feat-ex5-inspector-sections","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-ex5-inspector-sections` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
