# Task text-effects-multipass: Multi-pass text effects
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-text-effects-multipass.

## Issue / plan
Multi-pass text effects, the remainder of the text plan (#1 P5). Outline, shadow and softness already exist (PR #66); read that code first and build on it. APPROVED DESIGN: (1) The effect settings of TextRenderer become an ordered list of passes over the same glyph mesh. Each pass has its own colour, offset, SDF threshold and softness; passes draw back to front. (2) The order is explicit: shadow and outline passes first, the fill pass last. UIText and TextRenderer keep sharing Rendering::TextDrawing; do not fork it. (3) Software renderer: effect passes do not write depth and keep the 1e-5 depth bias from #66; only the fill pass writes depth. (4) No protocol change. Lua gets a binding for the pass list. (5) Tests are structural golden-image tests on the software renderer (as in #51/#66), ASCII-only. (6) The OpenGL path cannot be run in CI: list it as unverified on a GPU in the PR body. Keep the existing single-effect API working (map it onto the pass list) so current scenes and tests do not change behaviour. Docs: update the text docs page with the pass list, the draw order and a tradeoff callout for any choice you make. REWORK 2026-10-10 (user decisions on the first round, which is PR #110): (A) renderer/ edits are now allowed (renderer/ and text/ added to the allowed paths): a pass no longer has to be drawn through the shadow uniforms with face alpha 0 if a cleaner renderer/shader change is better; judge it, keep GL and software output identical to the first round for existing scenes. (B) Give TextPass a signed int `order` (default 0). The settings shadow and outline become entries of the same ordered list at fixed negative orders (shadow -200, outline -100); the list is stable-sorted by order, so a pass with order below -200 draws behind the settings shadow. The face is drawn after every pass with order <= 0. Defaults must reproduce the first-round order and image exactly, so existing scenes and tests do not change. Orders above 0 (draw in front of the face) are NOT implemented: reject or clamp them with one warning, and say so in the docs. (C) SERIALIZE the extra passes with the component in the scene file (TextRenderer and UI::UIText), including `order`, following how outlineWidth/shadowColor etc. are serialized now, and keep old scene files loading unchanged (missing passes = none). If scene serialization of a vector of structs or the editor reflection/inspector exposure would require a protocol.json change, do NOT make it: write needsUser. Prefab/duplicate/play-snapshot paths must carry the passes (add a test that a scene round trip and a play snapshot keep them). (D) Lua API stays AddEffectPass/ClearEffectPasses/GetEffectPassCount (accepted); add `order` to AddEffectPass. (E) Fix the two low review findings from round 1: the docs claim that the single-draw settings path equals the same list written as passes is only true when neighbouring glyph effects do not overlap (state that exactly, or make it true); WarnClamped must also name extra passes.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^renderer/
- ^text/
- ^docs/
- ^tests/
- ^CMakeLists.txt$

## Acceptance
- Tests for the new behaviour (order, serialization round trip, old scene loads, play snapshot keeps passes), ASCII-only, compile on all 4 CI jobs
- All 4 Tests-workflow jobs green
- docs/text.html has a 'Known limits and next steps' section that states: (1) the Lua pass API has no per-pass getter or setter (clear and re-add to change one), (2) negative width (an inner outline, an inset stroke, hollow text, engraving) is deferred: it needs passes that draw in front of the face (order > 0), an inside clamp of the SDF spread, and a software-renderer alpha-test/depth check, (3) orders above 0 are rejected/clamped. These are written so a later agent with only the brief and docs can pick them up.
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
PR #110 ALREADY EXISTS on this branch and its worktree exists (setup.mjs reuses it). Do NOT open a new PR: work on the existing branch, first merge origin/master into it, push, update the PR body (REST PATCH via the wrapper) and keep the same PR number in dev.json. The first-round review is in ledger/review.json (head 3d3e63d). User-approved and therefore NOT to be listed under deviations (list them in the PR body under 'Approved deviations' instead): SDF threshold is an outward width in ems with no negative values; settings-only text keeps the single shader draw; the Lua API has no per-pass getter. List in deviations only NEW departures or judgement calls beyond this note.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs text-effects-multipass` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-text-effects-multipass` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/text-effects-multipass/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-text-effects-multipass --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/text-effects-multipass/dev.json: {"pr":<number>,"branch":"feat-text-effects-multipass","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-text-effects-multipass` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
