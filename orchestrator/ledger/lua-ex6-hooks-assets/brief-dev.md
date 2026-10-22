# Task lua-ex6-hooks-assets: Lua editor extensions E-X6: hooks OnSceneSaved/OnSceneOpened/OnBeforePlay/OnAssetsChanged, Editor.Assets, the editor/ folder watcher
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-lua-ex6-hooks-assets.

## Issue / plan
Design: C:/Method Notes Obsidian/orchestrator/proposals/lua-editor-extensions.md (D1-D12 approved): read sections 4.2-4.5 (Editor API table incl. Editor.Assets around line 131, hooks table at section 4.5 around line 165, the asset watcher around line 194) and the stage table row E-X6 (line ~361) FIRST, then the merged E-X1..E-X5 code on master (editor-server/src/ExtensionHost.cpp, EditorServerExtensions.cpp, tests/editor-server/Extension*Tests.cpp) and docs/scripting-lua.html, docs/editor.html. SCOPE: (1) hooks OnSceneOpened(info), OnSceneSaved(info), OnBeforePlay(info), OnAssetsChanged(added, removed, modified) called by the host at the points in 4.5 (after SaveSceneFile succeeds and MarkSaved; after LoadOpenedScene; in HandleWritePlaySnapshot before the snapshot is written; from ApplyAssetChanges), each read-only (scene.read) under a SMALLER budget than invocations, run after the triggering command did its own work as a separate budgeted call, never inside scene load/deserialisation; a hook error is logged, counts towards the extension's fault flag and NEVER fails the save/open/snapshot; OnBeforePlay returns warnings that are surfaced to the client (prefer reusing the existing extensionOutput event with a level; add a protocol minor 1.17.0 ONLY if truly required, then follow the protocol-change rules: protocol.json, generated clients, golden vectors, docs, and say so in the report); never blocking play (D10); (2) Editor.Assets: List(folder), ReadText(path) (capability assets.read) and WriteText(path, text), CreateFolder(path) (assets.write), res:// only via the same resolver as the editor's asset commands, size caps, path traversal refused, writes go through the same edit/permission/read-only-mode rules as other Write-class operations and honour the role table; (3) the editor/ folder watcher: flag --editor-extensions-watch (OFF by default; only meaningful with --editor-extensions) polls editor/ like PollAssetsIfDue (mtime+size, then a content hash) and reloads only the changed file, with the extensionsChanged event; (4) tests: unit and host tests for every hook point (called, read-only, budget exceeded, error isolated, fault flag), Editor.Assets (caps, traversal, caps denied), the watcher (changed file only, hash unchanged = no reload), read-only mode; (5) docs: scripting-lua.html and editor.html sections verified against the code. Security-relevant (writes to project assets from Lua): fail closed, deny by default without the capability. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^editor-server/
- ^tests/
- ^docs/
- ^CMakeLists\.txt$
- ^cmake/

## Acceptance
- Four hooks called at the documented points, read-only, budgeted, errors isolated, never fail save/open/snapshot
- Editor.Assets with assets.read/assets.write capabilities, res:// only, traversal refused, caps enforced
- --editor-extensions-watch reloads only the changed file; off by default
- CI green incl. protocol check; docs verified against code
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer (security): capability gating for Editor.Assets, path traversal/symlinks, size caps, hook budget and fault isolation, hooks cannot edit, write-class rules and read-only mode honoured, watcher cannot load files outside editor/, protocol rules if 1.17.0 was added.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs lua-ex6-hooks-assets` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-lua-ex6-hooks-assets` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/lua-ex6-hooks-assets/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-lua-ex6-hooks-assets --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/lua-ex6-hooks-assets/dev.json: {"pr":<number>,"branch":"feat-lua-ex6-hooks-assets","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-lua-ex6-hooks-assets` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
