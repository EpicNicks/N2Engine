# Task demo-new-stations: lua_project demo: stations for particles, world-space canvas and the multi-pass text effects
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-demo-new-stations.

## Issue / plan
The user asked 2026-10-14: 'add stations for the new stuff' to the Lua demo (lua_project, the GPU smoke scene from 2026-10-06). Read first: lua_project/assets/scene.lua, lua_project/assets/scripts/SmokeStations.lua (how the existing stations are built and how the player/camera moves between them), lua_project/CMakeLists.txt and src/Main.cpp (how the demo starts, projectPath, physics.gpu), lua_project/project.n2proj, lua_project/tools/make_smoke_assets.py, the Lua annotations (engine/src/scripting/bindings/engine-api.lua) and the docs pages for each feature: docs/particles.html (ParticleSystem component, Lua Play/Stop/Clear/EmitBurst, backend Auto|PhysX|CPU, pbdMaterial Water/Sand/Smoke presets, Physics.IsGpuAvailable, Particles health row), the world-space canvas docs (PR #67: world canvas, Raycast QueryTriggers, hit-test limits), the text effects docs (outline, shadow, softness, multi-pass TextPasses ordering). Scope: add NEW stations next to the existing ones, in the same style (data-driven where the existing ones are; Lua only; no engine changes): (1) PARTICLES station: several emitters side by side: a continuous fountain (Water preset), a sand pile burst, a smoke column, each labelled with a TextRenderer sign that shows its preset and which backend is ACTIVE (read the component's/engine's reported backend and Physics.IsGpuAvailable(); show 'CPU' or 'PhysX GPU' live), a key/button prompt to EmitBurst, Stop/Play and Clear, and a key to cycle the backend field Auto -> CPU -> PhysX so the user can compare CPU and GPU (when no GPU, PhysX falls back to CPU with the engine's one warning: the sign must say so); particle counts shown live if the Lua API exposes them (check; do not add engine API). (2) WORLD-SPACE CANVAS station: a world canvas panel with a button/label the player can aim at and click with the existing raycast/pointer input (show hover + press feedback, and a counter), demonstrating that triggers do not block world canvases (a trigger volume in front of the panel, if the Lua API supports creating one: check). (3) TEXT EFFECTS station: signs showing outline, shadow, softness and multi-pass ordering (several TextRenderer instances with different effect passes, a before/after pair showing pass order), labelled with what they demonstrate. (4) Keep the existing stations and the scene start unchanged except for the minimum needed to reach the new stations (layout offset, a station list/teleport key if the existing code has one; follow its pattern). Assets: reuse the existing generated assets (tools/make_smoke_assets.py) and add new generated ones ONLY through that script if needed (committed outputs, like the existing ones). (5) CI: the demo must still build and any existing CI step that loads or smoke-runs lua_project (check tests/, tests/smoke, .github/workflows/tests.yml) must pass: if there is a headless smoke/golden for the demo scene extend it minimally to cover the new stations loading without Lua errors (e.g. run N frames headless on the software renderer and assert no error lines); do NOT add heavy new CI. (6) Docs: a short 'Demo' section/page update (find where lua_project is documented, README or docs/): list the stations, the controls (keys) for the new ones, what to expect with and without a GPU (CPU backend visible everywhere; PhysX GPU backend only on a Windows PC with an NVIDIA GPU and physics.gpu auto/on: say how the demo enables it, check how Main.cpp passes projectPath so physics.gpu applies: do not change the default off), and that particles render as billboards on OpenGL/software only (Vulkan draws nothing today). Note on dependency: particles PR-C (PhysX PBD backend, PR #129) is being merged separately; write the station against the documented component fields (backend Auto|PhysX|CPU, pbdMaterial) which already exist in master; if a field or Lua call does not exist in current master, adapt and list it in `deviations` instead of guessing. Nothing builds locally; CI is the build (8 checks, required `linux (Release, x86_64, Clang)`). Unverified by design: how it LOOKS on a real GPU (the user will view it).

## Allowed paths (regex, repo-relative)
- ^lua_project/
- ^docs/
- ^README\.md$
- ^tests/smoke/
- ^tests/lua_project/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All Tests-workflow jobs green; lua_project builds on every leg
- Particles, world-canvas and text-effects stations exist, labelled, with controls documented
- Existing stations and scene start unchanged except what is needed to reach the new ones
- No engine C++ or protocol change
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: check the Lua against the real API (engine-api.lua annotations and bindings: no nonexistent calls), existing stations untouched, no engine changes, docs list controls, the CPU/GPU backend sign logic is honest, no per-frame allocations that would hurt a demo, generated assets only via the script. Unverified: how it looks and behaves on a real GPU/window (the user views it).

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs demo-new-stations` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-demo-new-stations` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/demo-new-stations/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-demo-new-stations --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/demo-new-stations/dev.json: {"pr":<number>,"branch":"feat-demo-new-stations","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-demo-new-stations` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
