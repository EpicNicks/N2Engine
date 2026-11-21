# Issue #155 (web build umbrella): triage and a proposal to close or split

Status: a proposal. Nothing was posted to or closed on GitHub. Sources: `gh issue view 155` (body and one comment), master at
308f4f8b, `docs/web-build.html` (the "What is not done yet" section, line 662, and the sections it links), `docs/web-checklist.html`,
`web/budgets.json`, `cmake/N2EngineWeb.cmake`, `.github/workflows/tests.yml` (job `web`, line 1756), `orchestrator/proposals/webgpu-backend-design.md`
and `git log`. "W1 to W9" in the request are the nine WebGPU design decisions of `webgpu-backend-design.md` section 9 (all taken and
implemented), not umbrella stages; the umbrella's stages are 0 to 7, and WebGPU is split into G0 to G7.

## Verdict in one paragraph

The umbrella's stages 0 to 5 and 7 (up to G6) are merged and their CI-checkable acceptance items are met. Two stages are not: stage 6
(the threaded artifact; nothing exists in `cmake/` or `web/`) and the persistent-saves half of stage 3 / the filesystem blocker
(IDBFS; `docs/packs.html:219` says it is the gap). What is left of the rest is manual verification in real browsers
(`docs/web-checklist.html`), which the issue itself says needs the maintainer. Recommendation: **close #155 as delivered for its
"first version" acceptance, after opening four small issues** (list at the end), so the open work stops being hidden in a closed
umbrella's prose. If you prefer to keep it open, keep it open only as the manual-browser-verification tracker and retitle it.

## Acceptance for a first version (the issue's four bullets)

| Acceptance item | State | Evidence |
|---|---|---|
| CI builds the web target with Emscripten on every PR (docs-only PRs skip) and runs the headless unit tests under node | Done | Job `web (Emscripten, dev profile, node tests)` at `.github/workflows/tests.yml:1756`, `if: needs.changes.outputs.docs_only != 'true'` (line 1758). It runs math, text, assetimport, engine, renderer and web tests under node, then builds and smokes the package in all three profiles. It is not a required check (`docs/web-build.html` CI section; the branch-protection note is in `user-verification-checklist.md`). |
| `lua_project` (or a trimmed demo) runs in a browser with the software renderer, single-threaded, on plain static hosting: scene loads, scripts run, input works | Done in CI under node with a mock canvas; browser behaviour mostly unverified | PR #170 (`web/player/WebPlayer.cpp`, `web/shell.html`, `web/n2-loader.js`, `N2_WEB_PROJECT_DIR` default lua_project at `web/CMakeLists.txt:40`). Node smoke is `Smoke the web package under node` (`tests.yml:1982`). Real-browser output: the G5/G6 Chromium (SwiftShader) gate and the maintainer's Chrome/Edge hardware checks passed (`webgpu-backend-design.md` section 7, G5 row); Firefox, Safari, mobile, audible audio, touch and gamepad are open in `docs/web-checklist.html`. |
| Documented build profiles, output contract and hosting notes in `docs/` | Done | `docs/web-build.html`: profiles, `id="output-contract"` (line 571), size report and budgets (line 605), hosting recipes for precompressed files, `docs/web-checklist.html`. |
| Native Windows/Linux builds unchanged | Done (continuously) | Every native leg runs on every PR; web code is behind `N2ENGINE_PLATFORM_WEB` (`cmake/N2EngineWeb.cmake`). |

## Per stage

| Stage in #155 | State | Evidence | Open remainder |
|---|---|---|---|
| 0 Toolchain + CI smoke, `dev`/`size`/`speed` profiles | Done | PR #163; `cmake/N2EngineWeb.cmake` (`N2ENGINE_PLATFORM_WEB`, `N2ENGINE_WEB_PROFILE`); Emscripten 4.0.10 pinned in the `web` job; all three profiles built in CI. | None. |
| 1 Headless engine in WASM (core, scene, Lua, software goldens under node) | Done | PR #167; `N2ENGINE_SINGLE_THREADED` (`cmake/N2EngineWeb.cmake:29`), `RenderThread` single-thread branch, test exclusion list in `docs/web-build.html`. | ufbx (FBX/OBJ) is OFF on the web (`docs/web-build.html:154`, "Not attempted yet") and FreeType is forced OFF (line 153, stb_truetype only). Neither is an acceptance item; both are optional "feature toggles" in the issue's size profile. |
| 2 Software renderer to a canvas, window/input | Done in code, browser-unverified | PR #170. Later additions: touch (PR #332), gamepad (PR #357), the platform layer (no GLFW calls on the web). | `docs/web-build.html:667` still lists devicePixelRatio/HiDPI (done only for WebGPU, with a 1280-px cap for software), fullscreen and pointer lock, and a golden image of the web render as not done. The whole of `web-checklist.html` steps 1 to 7 has not been run by a human. |
| 3 Assets and packaging: pack, async loading, shell page, size budgets in CI, precompressed outputs | Mostly done | PR #174: loader with progress and MIME fallback, `manifest.json`, `.br`/`.gz`, `web/budgets.json` enforced by `scripts/web-budget-check.mjs` (`tests.yml:2012`), and the budgets have been raised by hand 10+ times with measurements recorded in the file. Packs and the VFS: build targets stage 4 (PR #338, `docs/packs.html`). `scripts/web-build.sh` builds a folder to host. | (a) Persistent saves: `user://` is MEMFS on the web, IDBFS is "the next step" (`docs/packs.html:219`). (b) Lazy fetch of single assets while the game runs (`docs/web-build.html:668`). (c) Time to first frame is not measured anywhere (needs a browser; the issue asks CI to "measure and report" it). (d) Trimming exceptions and RTTI from the size profile. |
| 4 Audio via Emscripten OpenAL / WebAudio | Done in code, unheard | PR #181; `docs/web-build.html:303`; loader unlock script tested under node (`tests.yml:2001`). | Nothing has been heard in a browser (`web-checklist.html` step 8). No mute control, no "click to enable sound" prompt, no demo sound. |
| 5 PhysX on WASM (CPU) behind the backend seam | Done | PR #184 (PhysX 5.6.1, scalar math, `PxDefaultCpuDispatcher` with 0 workers under `N2ENGINE_SINGLE_THREADED`, tests under node: `tests/web/PhysXWebTests.cpp`), `docs/web-build.html:356`. Also done beyond the issue: Jolt linked into the player (PR #205, line 635), 2D physics, joints. #156 is closed. | No browser measurement of frame time or memory (`web-checklist.html` step 9); a SIMD PhysX build, a prebuilt PhysX archive and a licence file inside the package are listed as not done (`docs/web-build.html:669`). |
| 6 Threaded build, second artifact chosen by `crossOriginIsolated` | Not started | No `-pthread`, no second artifact, no `crossOriginIsolated` selection in `cmake/`, `web/CMakeLists.txt` or `web/n2-loader.js`. `cmake/N2EngineWeb.cmake:28` says "a later threaded second artifact (stage 6)"; `docs/web-build.html:671` explains that the PhysX web libraries would need their own threaded build and cache key. | Everything. It is optional by the issue's own decision and needs real-browser measurement to justify (what does threading buy a 1280-px software render?). |
| 7 WebGPU backend | Done through G6 | G0 PR #188, G1 #190, G2 #196, G3a #201, G3b, G4 #208, G5 #231 (merged 2026-10-23), G6 Chromium SwiftShader gate (`773038f9`), user shaders on WebGPU (stage 3 and 9f of user shaders). WGSL, Tint and naga validation in CI; device loss and software fallback button; `webgpu-backend-design.md` decisions W1 to W9 all implemented. | G7 (pipeline pre-warm, async creation, compatibility feature level, wireframe emulation, performance work). Cross-browser, mobile and real driver-reset checks (`web-checklist.html:97`). |

Cross-cutting items from the issue body:

| Item | State | Evidence |
|---|---|---|
| Baseline needs no COOP/COEP; works from any path; wrong `.wasm` MIME handled | Done and CI-tested under node | `docs/web-build.html:381` and output contract; MIME fallback in `web/n2-loader.js`, `Test the web loader under node` (`tests.yml:1990`). Real browser and real host still unchecked (`web-checklist.html` step 5). |
| Size budget fails the build | Done | `web/budgets.json`, `scripts/web-budget-check.mjs`; "Report web package sizes" at `tests.yml:2007`. |
| Measure and report wasm size, gzip/brotli size, per profile | Done | Same step and `docs/web-build.html:605`. Time to first frame is the missing metric. |
| Networking excluded from the player | Superseded | The browser WebSocket transport to a native N2Server exists (`docs/web-build.html:685`, `tests/web/WebSocketClient.cpp`, CI step at `tests.yml:1956`). Not an open item. |
| Lua, sol2, ufbx, cgltf | Lua and cgltf done; ufbx off | See stage 1. |

## What a human still has to do (not a code gap)

All of `docs/web-checklist.html`: steps 1 to 9 and the WebGPU, particles and soft-body sections, on Chrome/Edge (partly done),
Firefox, Safari and a phone. This is the same class of work as #149 for the editor, and is the strongest argument for splitting:
a closed umbrella should not be the only record of it.

## Proposal

1. Open these issues (suggested titles, each short; link `docs/web-build.html#not-done`):
   - **Web: first browser verification pass** (the checklist in `docs/web-checklist.html`: Firefox, Safari, mobile, audio, touch,
     gamepad, hosting from a real static host, performance at N bodies). Maintainer-only; mirrors #149.
   - **Web: persistent saves via IDBFS** (mount at `user://`, sync after writes, a test under node with a mocked IDBFS, a note on the
     quota and private windows). Closes the filesystem blocker the umbrella listed. Small, code-only, the best next piece.
   - **Web: optional threaded artifact** (stage 6), labelled low priority/needs-measurement: write down first what threading would
     buy (software renderer rows, PhysX dispatcher) and keep the decision that the single-threaded baseline needs no headers.
   - **Web: load-time measurement and lazy asset fetch** (time to first frame in the G6 Chromium gate or the browser checklist; a pack
     that streams assets so first load is smaller; the exceptions/RTTI trim of the size profile). Groups stage 3's leftovers.
   Optional fifth, only if wanted: **Web: FBX/OBJ import (ufbx) and FreeType on the web**, a size-vs-feature decision.
2. Then close #155 with a comment saying: stages 0 to 5 and 7 (G0 to G6) are merged, the first-version acceptance is met in CI, and
   the remainder moved to the issues above. WebGPU G7 can be tracked under the first or its own issue when someone picks it up.
3. If you would rather not open issues now: leave #155 open, retitle it "Web build: browser verification and stage 6", and use this
   file as its checklist. Do not close it with the remainder unrecorded.

## What I could not verify

Whether the `web` job is green on master today (not queried; the recent commit log shows web budget PRs merging). Firefox, Safari and
mobile behaviour. Whether `docs/web-build.html` line 83 ("stages 0 to 5") and line 662's stage 2/3 "not done" text are stale against
touch (PR #332) and gamepad (PR #357); the page itself says touch and gamepads are done in other sections, so the "not done" list
should be refreshed when the split issues are opened (a docs change, not made here).
