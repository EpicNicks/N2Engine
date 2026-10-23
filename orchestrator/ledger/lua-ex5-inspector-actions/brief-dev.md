# Task lua-ex5-inspector-actions: Lua editor extensions E-X5 (engine): inspector sections declared by extensions and InvokeInspectorAction (0x83), protocol minor 1.16.0
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-lua-ex5-inspector-actions.

## Issue / plan
Lua editor extensions design C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md (D1-D12 APPROVED): read 4.1-4.4, 6.3, 6.4, 7.1 (the 0x83 row and the GetEditorUi inspectorSections shape) and the E-X5 row of 9 FULLY. MERGED: E-X1 sandbox profiles, E-X2 ExtensionHost (discovery, registry, GetEditorUi, protocol 1.14.0), E-X3 Editor.Scene API + InvokeMenuItem 0x82 with one edit group per invocation, all-or-nothing multi-command calls, rollback, extensionOutput, error codes staleUiRevision/editGroupOpen (protocol 1.15.0). Read editor-server's extension host code, InvokeMenuItem's implementation and tests, protocol.json and the generators FIRST: InvokeInspectorAction must reuse the same invocation machinery (edit group, rollback, budgets, nested dispatch with the invoker's role, audit line, staleUiRevision, editGroupOpen, faulted state) rather than duplicating it. SCOPE (engine side only; the frontend sync is a separate later PR): (1) the declaration API in extension files for inspector sections per the design (componentType, label, fields as FieldSchema, buttons with optional forms) captured at load, validated, and returned in GetEditorUi.inspectorSections (check what E-X2 already returns there and extend it); (2) InvokeInspectorAction 0x83 {componentId, actionId, params, uiRevision} -> ExtensionResult, ctx carries componentId and entityId, Write class, IsEditOnlyCommand (a play host refuses), classification table entry so the 'every command classified' test passes; minRole honoured like menu items; (3) protocol minor 1.16.0: protocol.json, regenerated clients via editor-server/protocol/generators, test-vectors.json, docs/logging-and-editor.html protocol section; (4) TESTS mirroring InvokeMenuItem's: one invocation = one undo step, failure rolls back, viewer forbidden, read-only refused, stale uiRevision refused, component id that does not exist or does not match the section's componentType refused, budget trip leaves the host answering, nested write gets the invoker's role, audit line; (5) docs: docs/editor.html and docs/scripting-lua.html (extension API: inspector sections). Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^editor-server/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$

## Acceptance
- Inspector sections declared, validated, returned by GetEditorUi
- InvokeInspectorAction 0x83 reuses the InvokeMenuItem machinery; classified Write and edit-only
- Protocol 1.16.0 with regenerated clients and vectors
- Tests listed in the brief pass; all checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security/protocol): role checks, componentId/componentType binding, no duplicate invocation path that skips budgets or the edit group, classification, generators re-run (no hand edits), vectors, docs true. Mediums count.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs lua-ex5-inspector-actions` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-lua-ex5-inspector-actions` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/lua-ex5-inspector-actions/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-lua-ex5-inspector-actions --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/lua-ex5-inspector-actions/dev.json: {"pr":<number>,"branch":"feat-lua-ex5-inspector-actions","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-lua-ex5-inspector-actions` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
