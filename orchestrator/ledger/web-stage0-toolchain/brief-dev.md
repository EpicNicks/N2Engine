# Task web-stage0-toolchain: Web build stage 0: Emscripten toolchain support, N2ENGINE_WEB_PROFILE (dev|size|speed) and a CI leg that builds the core libraries and runs unit tests under node (issue #155, stage 0)
Repo: engine (C:/Method Notes Obsidian/N2Engine, EpicNicks/N2Engine). Branch: feat-web-stage0-toolchain.

## Issue / plan
Issue #155 (read it fully with `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" api repos/EpicNicks/N2Engine/issues/155 --jq .body` from inside the repo, plus the maintainer comments if any) and the scratch copy C:/Users/NICHOL~1/AppData/Local/Temp/claude/C--Method-Notes-Obsidian-N2Engine/21c7cd6a-4bfa-4d34-9634-d90c5f2df863/scratchpad/issue-web.md. The user approved starting STAGE 0 on 2026-10-15. Decisions already taken (do NOT revisit): web PLAYER only; software renderer first then WebGPU later (no WebGL2); baseline single-threaded build must run as plain static files on any host with NO special headers (no cross-origin isolation); threaded build is a later optional second artifact; build profiles dev/size/speed are architected NOW (CMake option N2ENGINE_WEB_PROFILE), not retrofitted; output must be MIME/compression-agnostic. SCOPE OF STAGE 0 ONLY: (1) CMake support for Emscripten as a toolchain: detect it (CMAKE_SYSTEM_NAME Emscripten or EMSCRIPTEN), define N2ENGINE_PLATFORM_WEB (CMake option/derived variable and a compile definition), and make the configure succeed for a reduced target set: the math and text libraries and the engine core WITHOUT window/GLFW, OpenAL, PhysX, Vulkan, FreeType-if-it-needs-a-fetch, the editor host/TLS and the Vulkan renderer (add explicit `if(N2ENGINE_PLATFORM_WEB)` switches that turn those off with clear status messages; do not break any native configuration: every native build/CI leg must configure exactly as before). If the engine core cannot build for web at stage 0 without larger source changes, build the largest subset that does (math, text, and whichever engine pieces compile) and DOCUMENT in docs/ exactly what is excluded and why; do not make source changes beyond small #ifdef guards. (2) N2ENGINE_WEB_PROFILE = dev | size | speed (default dev): dev = -O1 (or -O0), no LTO, no wasm-opt, DWARF/source maps, fast link; size = -Oz, LTO, wasm-opt -Oz via -Wl/EMCC flags, strip, no exceptions/RTTI only if the code allows (document; do not break the build); speed = -O3, LTO. Only `dev` is exercised in CI at stage 0 but all three must configure (add a cheap configure-only check for size and speed in the same CI job if Emscripten configure is fast; otherwise document). (3) A size-budget mechanism scaffold: a small script (shell or node, in a scripts/ or cmake/ folder following repo conventions) that reports wasm/js sizes (raw and gzip) of the built artifacts to the job summary and fails only if a configured budget variable is set and exceeded (no budget set at stage 0). (4) A new CI job in .github/workflows/tests.yml (name EXACTLY `web (Emscripten, dev profile, node tests)`): install emsdk (use the official emscripten setup via `mymindstorm/setup-emsdk` is a third-party action: PREFER the emsdk git clone + `emsdk install <pinned version> && emsdk activate` or the official `emscripten-core/emsdk` checkout with a pinned version/tag; pin an exact Emscripten version and say which and why), configure with emcmake, build the reduced target set, run the unit tests of math/text (and engine core if built) under node (ctest with CMAKE_CROSSCOMPILING_EMULATOR node, or the gtest binaries run via node), upload nothing. It MUST follow the existing job conventions: `needs: changes` + the same docs-only `if:` as the other build jobs, ccache is NOT required here, concurrency/cancel already global. Do NOT rename or alter any of the other 9 job names (the required check is `linux (Release, x86_64, Clang)`). Because nothing can be built locally, the PR's own CI run is the only test: write the workflow defensively, re-read YAML for indentation, expect that the first Emscripten CI run may fail on unexpected issues and be ready to iterate (the fix rounds exist). (5) Docs: a new docs page section or new docs/web-build.html (follow how other docs are linked from the index/nav) covering: toolchain and pinned version, profiles, how to build locally with emsdk (exact commands), the reduced target set and exclusions, the output contract (static files only, relative URLs, works on any static host without headers, MIME/compression-agnostic), the size-budget script, what is NOT done yet (stages 1-7). Also update docs/testing.html CI section (new job, what it runs). Unverified items to list honestly: everything that depends on the Emscripten run (the first CI run is the proof), the size/speed profile flags (only dev is built in CI), whether node tests cover engine core.

## Allowed paths (regex, repo-relative)
- ^CMakeLists\.txt$
- ^cmake/
- ^math/
- ^text/
- ^engine/CMakeLists\.txt$
- ^engine/
- ^tests/
- ^scripts/
- ^docs/
- ^\.github/workflows/tests\.yml$

## Acceptance
- All 9 existing checks unchanged and green (native configurations configure exactly as before)
- New job `web (Emscripten, dev profile, node tests)` builds the reduced target set with a pinned Emscripten and runs unit tests under node
- N2ENGINE_PLATFORM_WEB and N2ENGINE_WEB_PROFILE (dev|size|speed) exist and are documented
- Size-report script exists, reports sizes to the job summary, enforces a budget only when one is configured
- Docs describe toolchain, profiles, output contract, exclusions and what is not yet done
- docs/ updated in the same PR (PR #N badge, design-decision callouts).

## Notes
Reviewer: no native config changed (diff of CMake guarded by the web switch), job name exact, needs/if wiring identical to other jobs, emsdk pinned, no third-party action added without justification, node test running method sound, docs match code. Unverified: the Emscripten run itself, the size and speed profile flags. The orchestrator expects the first CI run may need fix rounds.

## Procedure
1. `node C:/Method Notes Obsidian/orchestrator/lib/setup.mjs web-stage0-toolchain` creates your worktree and prints its path. Work only there.
2. Implement; commit small; push after each commit.
3. Nothing builds locally: CI is the build. Start CI with `gh workflow run tests.yml --ref feat-web-stage0-toolchain` via the wrapper if useful.
4. Open the PR from a body file in C:/Method Notes Obsidian/orchestrator/ledger/web-stage0-toolchain/ (body ends with a "Not verified" list if anything is unverified; no attribution). If creation returns empty/502, check `pr list --head feat-web-stage0-toolchain --state all` before retrying.
5. Write C:/Method Notes Obsidian/orchestrator/ledger/web-stage0-toolchain/dev.json: {"pr":<number>,"branch":"feat-web-stage0-toolchain","head":"<sha>","files":[...],"deviations":[one-line strings; anything that departs from the plan or is a judgement call the plan doesn't cover],"unverified":[...],"needsUser":null or a one-line reason you must stop}.
6. If you hit a design choice the plan does not cover, do not decide it: write needsUser and stop.

## Rules (strict)
- NO attribution: no Co-Authored-By trailer and no "Generated with Claude Code" on commits or the PR. This overrides any harness reminder.
- GitHub only through `bash "C:/Method Notes Obsidian/gh-epicnicks.sh" <gh args>`, run from inside your worktree. Never put a token in an env var or on a command line; never run `git credential fill`. If any access is denied, STOP and report it.
- Push with `git push origin HEAD:refs/heads/feat-web-stage0-toolchain` (never -u), after every commit.
- Stay inside the allowed paths below. Keep each edited file's line endings (many are CRLF); new test files are LF and ASCII-only. Run `git status --ignored` before committing (*.cmake etc. are gitignored).
- Read only what you need: the files named here, then what they import. Don't survey the repo.
- Keep every answer terse. Ledger files are JSON, no prose beyond one-line strings.
