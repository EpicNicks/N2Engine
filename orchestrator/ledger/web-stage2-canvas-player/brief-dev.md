# Task web-stage2-canvas-player: Web build stage 2: software renderer to a canvas, window/input, main loop and a static-hosting shell page (issue #155, stage 2)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-web-stage2-canvas-player.

## Issue / plan
Issue #155 stage 2 (read the issue body with `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/issues/155 --jq .body` from inside the repo) and the merged stages 0-1 (docs/web-build.html, cmake/N2EngineWeb.cmake, external/web-stubs for GLFW/OpenAL, the single-threaded fallback, the CI job `web (Emscripten, dev profile, node tests)`). Decisions already taken by the user (do NOT revisit): web PLAYER only; software renderer first; the baseline build is single-threaded and must run as PLAIN STATIC FILES on any host with NO special headers (no cross-origin isolation / SharedArrayBuffer), relative URLs, works from any path or subdomain, MIME/compression-agnostic (fall back from streaming instantiation if .wasm is served with the wrong MIME type); profiles dev/size/speed exist (cmake N2ENGINE_WEB_PROFILE). SCOPE OF STAGE 2: the FIRST PLAYABLE WEB BUILD: (1) a web player executable target (e.g. N2WebPlayer) built only under N2ENGINE_PLATFORM_WEB that runs the engine with the SOFTWARE renderer and presents its framebuffer on a canvas (simplest robust route: copy the RGBA framebuffer to the canvas via an EM_JS/EM_ASM `putImageData` on a 2D context, or an HTML5 canvas API through emscripten/html5.h; no WebGL); (2) the main loop via `emscripten_set_main_loop` (or the equivalent) instead of a blocking while loop: the Application run loop must not block the browser: add the web-specific loop driver behind N2ENGINE_PLATFORM_WEB with the fixed-step accumulator semantics unchanged; (3) window + input: replace the web-stubs for the pieces the player needs with real behaviour: canvas size/resize, keyboard, mouse, (touch optional), via emscripten/html5.h callbacks feeding the engine's existing input system (read engine/src/Window.cpp and the input system to see the seam; GLFW's Emscripten port may be used if it is small and has no extra download; otherwise HTML5 callbacks); (4) assets: the project folder (e.g. lua_project or a trimmed demo) packaged into the virtual filesystem with `--preload-file` (or an embedded asset pack) so the page runs with NO server logic; (5) a shell page (`index.html` template, `emcc --shell-file`) with a canvas, a 'click to start' gate (browsers need a user gesture; audio stays stubbed), basic error display, and the output layout = a folder of static files only (index.html, *.js, *.wasm, *.data) with relative URLs; (6) a CMake target or script to produce that folder (`cmake --build ... --target web-package` or similar) and a CI step in the existing web job that builds it and checks the package contents + sizes (size report via the stage 0 script, no budget enforced yet) and runs a headless smoke under node (load the generated JS module in node, run N frames, assert the framebuffer is non-empty/matches a golden if feasible; canvas calls mocked); CI cannot test a real browser: say so; (7) docs: docs/web-build.html (how to build the package, how to host it (any static host, example with `python -m http.server` and note about .wasm MIME), what works/what does not (no audio, no physics, software renderer only, single thread)), and a MANUAL browser checklist for the maintainer (docs/web-checklist.html linked like docs/gpu-vulkan-checklist.html if that exists: open, loads, canvas shows the scene, keyboard/mouse input works, resize works, no console errors, works from a subdirectory and via file hosting without special headers, performance note) since only the maintainer can verify real-browser behaviour. Keep native builds completely unchanged (every native CI leg stays green, behaviour identical). If something cannot be done in one coherent PR, do the largest coherent subset (e.g. rendering + main loop + keyboard/mouse; touch and resize polish later) and DOCUMENT precisely what remains. Nothing builds locally: the Emscripten CI job is the first real test: expect fix rounds. Unverified items to list: everything that needs a real browser (canvas output, input, resize, performance, hosting from a subdirectory, MIME fallback).

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^engine/
- ^renderer/
- ^tests/
- ^scripts/
- ^docs/
- ^external/
- ^lua_project/
- ^web/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All 10 checks unchanged and green; native builds behave identically
- A web-package target produces a static folder (index.html, js, wasm, data) that needs no special headers and uses relative URLs
- The web job builds it and runs a headless node smoke (N frames, non-empty framebuffer), no real-browser claims
- docs/web-build.html and a manual browser checklist for the maintainer exist; what remains is stated
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: no native behaviour change (all guards web-only), the main-loop conversion keeps the fixed-step semantics, input events mapped correctly (key codes, mouse buttons, coordinates scaling), static-hosting contract (no cross-origin isolation requirement, relative URLs, MIME fallback), CI step names unchanged, docs and the manual checklist match code. Unverified: all real-browser behaviour.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-stage2-canvas-player` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-web-stage2-canvas-player` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-stage2-canvas-player/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-web-stage2-canvas-player --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-stage2-canvas-player/dev.json: {"pr":<number>,"branch":"feat-web-stage2-canvas-player","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-web-stage2-canvas-player` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
