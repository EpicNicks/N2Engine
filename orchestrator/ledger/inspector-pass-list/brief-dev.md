# Task inspector-pass-list: Inspector: a pass-list editor for text effect passes
Repo: frontend (C:/Method Notes Obsidian/N2Engine-Electron-Frontend, EpicNicks/N2Engine-Electron-Frontend). Branch: feat-inspector-pass-list.

## Issue / plan
Approved by the user 2026-10-10 (design A, part 2 of 2). In the inspector, a field whose kind is Json and whose `typeName` is "TextPass[]" (the engine sets this for TextRenderer/UIText `effectPasses` in the sibling engine item passes-typename-hint; against an older engine the field stays the raw Json textarea, which is fine) gets a `PassListEditor` instead of the Json textarea. The value is an array of {color:{r,g,b,a}, offset:{x,y}, width, softness, order}; missing keys take engine defaults (color black opaque, others 0). Each row shows colour, offset x/y, width, softness and order, with remove and move-up/move-down buttons; an "Add pass" button appends a default pass. Build rows from the existing ColorEditor, VectorEditor and NumberEditor (src/renderer/inspector-editors.tsx; RefListEditor is the closest list template). Every edit commits the whole array through the EditorProps commit(value, final) like other editors; number drags use the existing gesture handling so a drag is one undo step. `order` is an integer; values above 0 are clamped to 0 by the engine, so show a short hint under the field (orders above 0 are not supported yet) and clamp nothing in the UI. `width` and `softness` are not negative in the engine: set min 0 on those number inputs. Dispatch in FieldEditor (~line 804) on typeName, not on the field name. Tests (vitest): the editor renders N rows for N passes, add appends a default pass, remove and reorder produce the right array, an edit commits the whole array, a non-array or malformed value falls back to the Json textarea, and the dispatch picks PassListEditor only for typeName "TextPass[]". No engine-ref or protocol change.

## Allowed paths (regex, repo-relative)
- ^src/
- ^README.md$

## Acceptance
- npm run typecheck, npm run build and npm test pass locally
- Tests for the editor as listed in the plan

## Notes
No CI in this repo: local typecheck, build and tests stand in. If the inspector section of README.md describes field editors, add one line for the pass list. Cannot be checked against a live host from here: list "not run against a live host" under unverified.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs inspector-pass-list` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Run npm run typecheck, npm run build, npm test locally.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/inspector-pass-list/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-inspector-pass-list --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/inspector-pass-list/dev.json: {"pr":<number>,"branch":"feat-inspector-pass-list","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-inspector-pass-list` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
