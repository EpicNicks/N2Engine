# Task lua-ex7-docs-example: Lua editor extensions E-X7: docs/editor-extensions.html, editor-api.lua annotations, example lua_project/editor/AlignTools.lua
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: docs-lua-ex7-extensions.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md (D1-D12 approved), row E-X7 of the stage table (line ~362). E-X1..E-X6 are merged: read the CODE as the source of truth (editor-server/src/ExtensionHost*.cpp, ExtensionHost.hpp, tests/editor-server/Extension*Tests.cpp, tests/editor-server/project/EditorExtension*Tests.cpp, the existing docs/scripting-lua.html, docs/editor.html, docs/logging-and-editor.html protocol sections, the engine-api.lua file and how it is produced/checked). SCOPE: (1) new docs/editor-extensions.html in the style of the other docs pages (nav entry in docs/index.html and the shared nav of other pages as they do for new pages): the model (Lua in the edit host, UI is data, no code in Electron), enabling (--editor-extensions, trust prompt, --editor-extensions-watch), file layout (<project>/editor/*.lua), the full Editor API as implemented (menu items, forms and field kinds, inspector sections and actions, hooks OnSceneOpened/OnSceneSaved/OnBeforePlay/OnAssetsChanged, Editor.Scene/Selection/Assets/Log/Undo etc: list exactly what the code registers), capabilities and the role table, budgets and fault flag, the sandbox, security notes (trust, Admin-only, what is never available), troubleshooting (extension states: loaded, refused, faulted, why), and a worked example; (2) editor-api.lua: LuaLS annotations for the Editor API, separate from engine-api.lua (check how engine-api.lua is maintained and whether a test or generator checks it against the registered API; if a check exists, extend it to editor-api.lua; if none, add a cheap test that every function registered by the host appears in editor-api.lua and vice versa); (3) example lua_project/editor/AlignTools.lua: a small, useful, safe example (e.g. align/distribute selected objects along an axis with a form, plus an inspector section or hook demonstrating the API), with a project test that loads it through the host and runs its menu item (use the existing extension test fixtures); it must be OFF by default (extensions flag) and must not change existing lua_project behaviour or tests; (4) update docs/editor.html role text and docs/scripting-lua.html#sandbox to match the code; (5) EVERY sentence verified against the code (this is the hard part: docs verification cost many review rounds before): when the code and an older doc disagree, the code wins and you fix the older doc. Docs/CI: if a docs check exists (tests/docs or workflow step), run its logic by reading and make sure it would pass. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^docs/
- ^lua_project/
- ^engine/
- ^editor-server/
- ^tests/
- ^\.github/
- ^[^/]*\.lua$

## Acceptance
- docs/editor-extensions.html complete and true against the code, linked from the index
- editor-api.lua annotations for every registered Editor function, checked by a test
- AlignTools.lua example loads and runs under the host in a test
- editor.html and scripting-lua.html updated; CI green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: verify docs sentence by sentence against ExtensionHost code (capabilities, role gating, budgets, hook semantics, trust flow, flags), the annotations match the registered API exactly, example is sandbox-safe and off by default, no behaviour change.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs lua-ex7-docs-example` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref docs-lua-ex7-extensions` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/lua-ex7-docs-example/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head docs-lua-ex7-extensions --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/lua-ex7-docs-example/dev.json: {"pr":<number>,"branch":"docs-lua-ex7-extensions","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/docs-lua-ex7-extensions` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
