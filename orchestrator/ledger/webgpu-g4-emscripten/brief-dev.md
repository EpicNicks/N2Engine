# Task webgpu-g4-emscripten: WebGPU G4: Emscripten build of the WebGPU backend with emdawnwebgpu, --renderer argument and RenderBackend::WEBGPU, windowless WebGPU path in Window, backend compiled into the web player (still started on software), budgets re-measured
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-webgpu-g4-emscripten.

## Issue / plan
WebGPU design C:/Method Notes Obsidian/orchestrator/proposals/webgpu-backend-design.md (W1-W10 ACCEPTED): read sections 2, 3, 4.4, 5 and the G4 row of 7 FULLY plus 8 risks. MERGED: G0 shared GPU pieces, G1 WGSL + validation, G2 native harness (Dawn on lavapipe, non-required 'webgpu (Dawn, lavapipe)' job), G3a resources + unlit, G3b lit/blend/text; the web build stages 4/5 and the size budgets (web/budgets.json; NOTE the Jolt J3 PR #205 is about to raise web/budgets.json too: merge origin/master before your final push and re-measure on top of it). The WebGpuRenderer must compile for Emscripten against webgpu.h only (the emdawnwebgpu port, pinned): no Dawn-native-only APIs in the shared code path (guard the native-only bits: e.g. synchronous readback natively vs the async path on the web). SCOPE exactly the G4 row: (1) the emdawnwebgpu port in the web build (pinned, documented; verify the flag/port name for the Emscripten version CI uses), (2) a --renderer argument (software|webgpu, and the native values that exist) and RenderBackend::WEBGPU in the enum with selection logic, (3) the windowless WebGPU path in Window for the web, (4) the backend compiled into the web player but the player still STARTS ON SOFTWARE (canvas attach, present and loader selection are G5, not here), (5) web link in all three profiles, the node smoke still green on software, (6) budgets re-measured and raised only by what the numbers show, with the before/after numbers in the PR body, (7) docs: docs/web-build.html (the WebGPU build part, what is not wired yet), docs/rendering docs where --renderer is described. Nothing builds locally for the engine; CI (12 checks) is the test; expect fix rounds; re-read every edit twice (CMake/YAML/C++ syntax); NEVER call a stateful function (RNG etc.) twice in one argument list or operator expression (unspecified evaluation order); NEVER run taskkill or kill other processes; verify every docs sentence against the CODE; no attribution lines on commits or PRs (no Co-Authored-By, no Generated-with footer); GitHub only via the wrapper; before your final push merge origin/master into the branch if it moved; your FINAL message must be a real report (PR number, head sha, what you did, deviations), never a single word.

## Allowed paths (regex, repo-relative)
- ^engine/
- ^renderer/
- ^tests/
- ^docs/
- ^web/
- ^CMakeLists\.txt$
- ^cmake/
- ^\.github/workflows/tests\.yml$
- ^player/
- ^editor-server/

## Acceptance
- Web player links the WebGPU backend in all three profiles via emdawnwebgpu (pinned)
- --renderer argument and RenderBackend::WEBGPU exist; web still starts on software; node smoke green
- Budgets re-measured with numbers in the PR body
- Native WebGPU job still green; all checks green; docs verified
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: no native-only Dawn API leaks into the web path, the async readback path, emdawnwebgpu pin, budgets raised only by measured amounts, the player does not start on WebGPU yet, --renderer validation and errors, docs true.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs webgpu-g4-emscripten` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-webgpu-g4-emscripten` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g4-emscripten/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-webgpu-g4-emscripten --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/webgpu-g4-emscripten/dev.json: {"pr":<number>,"branch":"feat-webgpu-g4-emscripten","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-webgpu-g4-emscripten` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
