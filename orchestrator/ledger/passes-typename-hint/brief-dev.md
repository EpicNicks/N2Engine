# Task passes-typename-hint: Text effect passes: typeName hint "TextPass[]" in the reflection schema
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-passes-typename-hint.

## Issue / plan
Approved by the user 2026-10-10 (design A, part 1 of 2). The TextRenderer and UI::UIText member `effectPasses` (std::vector<Text::TextPass>) is registered with RegisterMember and so is a Json field with typeName "json" (FieldTraits default, engine/include/engine/serialization/FieldInfo.hpp; JSON shape in engine/include/engine/text/TextJson.hpp). Make the reflection schema report typeName "TextPass[]" for it, keeping kind Json and the stored/serialized JSON unchanged, so the editor can recognise the field and draw per-pass widgets. Smallest way: a FieldTraits specialization for std::vector<Text::TextPass> (kind Json, TypeName "TextPass[]"), or FieldBuilder if that is how typeName is overridden elsewhere; follow what the code already does. NO protocol.json change, no version bump, no new FieldKind (typeName is already a free string on the wire). Do not change scene serialization or the Lua API. Tests: a reflection test that both components report kind Json and typeName "TextPass[]" for effectPasses, and that a value still round-trips. Docs: one sentence plus the typeName value where the docs list reflection/inspector typeNames (docs/ search for typeName), and in docs/text.html Known limits: the inspector draws a pass list editor for this typeName.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^docs/
- ^tests/

## Acceptance
- Reflection test for the typeName, ASCII-only, compiles on all 4 CI jobs
- All 4 Tests-workflow jobs green
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Keep it small: this is a one-to-three file change. If another component or a test snapshot lists typeNames and changes as a result, update it. If you find the typeName cannot be overridden without a protocol change, write needsUser and stop.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs passes-typename-hint` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-passes-typename-hint` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/passes-typename-hint/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-passes-typename-hint --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/passes-typename-hint/dev.json: {"pr":<number>,"branch":"feat-passes-typename-hint","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-passes-typename-hint` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
