# Proposal: make the N2Engine Tests workflow faster

Status: research only, nothing changed in the repo. Marks: [V] verified (repo at origin/master a1aa100, or measured through the
GitHub jobs API on 2026-10-15); [M] assumption or from memory of GitHub/CMake/ccache docs, UNVERIFIED until a CI run proves it.

GitHub API use: 4 requests (runs for a1aa100, jobs of run 38732431586, the last 40 Tests runs, jobs of run 38725839772). No logs downloaded.

## 1. Measurements

### 1a. One uncontended run: master push a1aa100 (merge of PR #147), run 38732431586, 28.4 min wall [V]
Every job started within 1-4 s of creation (no queueing).

| Leg | Job total | Checkout | Deps (apt / Vulkan SDK) | PhysX cache | Configure | **Build** | ctest | Smoke tests |
|---|---|---|---|---|---|---|---|---|
| windows (Release, PhysX ON) | **28.4 min** | 6 s | 9 s | 6 s | 53 s | **1576 s (26.3 min)** | 23 s | 19 s |
| windows (Debug, PhysX ON) | 16.1 min | 8 s | 16 s | 7 s | 54 s | 790 s (13.2 min) | 47 s | 23 s |
| windows (Debug, PhysX OFF, FreeType/ufbx/TLS ON) | 13.8 min | 10 s | 19 s | skipped | 56 s | 670 s (11.2 min) | 50 s | 19 s |
| linux (Release, x86_64, Clang) **required** | 13.7 min | 2 s | 22 s | 1 s | 10 s | 739 s (12.3 min) | 34 s | 11 s |
| linux (Release, arm64, Clang) | 11.3 min | 2 s | 18 s | 1 s | 12 s | 596 s (9.9 min) | 33 s | 11 s |
| linux (Release, x86_64, GCC 14) | 14.1 min | 1 s | 20 s | n/a | 19 s | 769 s (12.8 min) | 24 s | 11 s |
| docker (editor host image) | 9.7 min | 2 s | (inside build) | (inside build) | | 575 s (9.6 min, `docker build`) | | 2 s |
| protocol | 0.1 min | 3 s | 1 s | | | | 1 s | |

- **Critical path: `windows (Release, PhysX ON)`**, and within it the Build step (93 % of the job). Next longest is Windows Debug PhysX ON at 16 min, then the Linux legs at 11-14 min. [V]
- The compile is everything: setup + configure + test + smoke is 1-2.5 min per leg; the build is 85-93 % of every leg. [V]
- Windows Release builds 2x slower than Windows Debug (26 vs 13 min) on the same sources: the MSVC optimiser on this code (sol2 bindings, 87 KB PhysXBackend.cpp, /bigobj engine) dominates. [V times; cause M, no logs read]

### 1b. A contended run: PR #147 head 0123d3b, run 38725839772, 48.0 min wall [V]
| Leg | Queued before start | Ran | Build |
|---|---|---|---|
| windows Release | **19.5 min** | 28.5 min | 26.3 min |
| windows Debug PhysX ON | 21.2 min | 18.6 min | 15.2 min |
| windows Debug OFF/TLS | 21.0 min | 17.0 min | 14.2 min |
| linux x86_64 Clang | 8.9 min | 10.5 min | 9.2 min |
| linux arm64 | 17.4 min | 10.8 min | 9.3 min |
| linux GCC | 9.2 min | 13.8 min | 12.6 min |
| docker | 8.8 min | 8.5 min | 8.4 min |
| protocol (6 s of work) | 21.3 min | 0.2 min | |

- Run time was 28.5 min; **19.5 min of the 48 was queueing**. Between 11:10 and 11:16 six Tests runs were created (master 14d466e, feat-softbody-physx 1b50312, and four pushes of docs-particles-gpu-verified: 6420e32, 2126ddf, 0123d3b, 08c384c) = 48 jobs, against the account's concurrent-job limit (20 for a Free account [M]). [V runs; limit M]
- The three superseded docs-particles runs all ran to completion (23.5, 42.1, 48.0 min): there is **no `concurrency:` block in tests.yml** [V], so an older head of the same PR keeps all 8 slots busy.
- That PR changed only `docs/*.html` (4 files, 15 lines) [V `git diff --stat a1aa100^1 a1aa100`], yet each of its 4 pushes compiled the engine 6 times. No test reads docs/ (only comments mention it) [V grep].

### 1c. Last 40 Tests runs (2026-10-14 23:25 to 2026-10-15 12:08) [V]
- Uncontended runs: 27.6-30.1 min (Windows Release floor). Contended runs: 37.8-51.7 min. Longest: 51.7 min. No single run reached 60 min.
- So the "60-80 min per change" the orchestrator sees is [M, inferred] one run plus queueing, plus a second full run after "Update branch" (each merge to master starts a master run and, with strict up-to-date protection, a re-run on every other open PR), plus superseded runs that still hold slots.
- 7 of 40 runs were cancelled by hand; 5 more were superseded-but-completed (6420e32, 2126ddf, 0123d3b, 0eb3dd4, c333529): about 40 wasted job slots of 6-28 min each in 13 hours.

## 2. Why the build is slow (repo facts)

- **No compiler cache anywhere** [V]: no ccache/sccache, no `CMAKE_<LANG>_COMPILER_LAUNCHER`, no `actions/cache` of anything but PhysX binaries. Every leg compiles every TU from scratch on every run, including OpenAL Soft, googletest, Mbed TLS and Lua that never change.
- **Translation units** (own sources, `git ls-tree` [V]): engine 105, tests/engine 99, tests/editor-server 33, tests/renderer 13, editor-server 10, renderer 11, text 9, assetimport 8, math 7, tests (math/text/assetimport) 17, test_project 4, lua_project 1: about 317. Vendored C/C++: mbedtls 109 (only when TLS ON), lua 34, glad/mikktspace/stb_vorbis/ufbx 4. Fetched: OpenAL Soft 1.24.0 (~150-200 TUs [M]) and googletest (~4). Total ~500-650 TUs per leg.
- **Heavy headers**: `external/sol2/include/sol/sol.hpp` is 1.0 MB single header [V], included through 6 public engine scripting headers (LuaComponent.hpp, LuaRuntime.hpp, LuaBindings.hpp, ...) [V]; engine is built with `SOL_ALL_SAFETIES_ON=1` [V]. Every TU that reaches those headers parses sol2 again; **no precompiled headers and no unity build** anywhere [V grep].
- **Windows uses the Visual Studio generator** (`cmake -S . -B build -A x64`, no `-G`) [V]. `/MP` is on for every MSVC target (`CMakeLists.txt:17`) [V] and `cmake --build --parallel` adds MSBuild `/m` [M], so up to 4 projects x 4 cl.exe can run on the 4-vCPU runner [M]: oversubscription, and no cross-project scheduling like Ninja's. The VS generator does **not** support `CMAKE_CXX_COMPILER_LAUNCHER` [M, CMake docs: Makefile and Ninja generators only], so ccache/sccache need a Ninja generator on Windows.
- **Windows builds ALL targets** (no `--target`), including test_project and lua_project; Linux builds a named list of 8 targets [V]. Small difference (5 TUs + post-build copies).
- **Debug info**: `cmake_minimum_required(VERSION 3.25)` [V] so policy CMP0141 is NEW [M]: Debug uses `/Zi` (shared PDB, not cacheable by ccache/sccache), but `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` (= `/Z7`) can be set on the configure line without editing CMakeLists. Release has no compile-time debug info by default (`/O2 /Ob2 /DNDEBUG`) [M], so Release is cacheable as is.
- **FetchContent is not cached** [V]: OpenAL is a `GIT_REPOSITORY` clone without `GIT_SHALLOW` (full history) [V], googletest is a URL zip [V]. Cost shows mostly on Windows configure (53-90 s vs 10-19 s on Linux) [V]; part of that is the VS generator's compiler detection [M].
- **Tests**: 7 ctest entries, one `add_test` per gtest executable (no `gtest_discover_tests`) [V]; ctest runs serially. ctest is 23-63 s per leg [V], so `ctest -j` is worth seconds, not minutes.
- **Shaders**: 6 GLSL files through glslangValidator [V]: negligible.
- **Docker**: `COPY . .` before the build [V], so any source change invalidates the build layer; the image recompiles N2EditorHost from scratch with the same flags as the Linux Clang leg (duplicate work), and nothing is cached between runs (plain `docker build`, no buildx cache) [V].
- **Cross-leg sharing on Windows is limited**: `N2ENGINE_PHYSX_ENABLED=1` and `N2ENGINE_DEBUG=1` are PUBLIC defines on `engine` [V], so engine, editor-server and all engine-dependent tests differ between Debug ON / Debug OFF / Release. Only math, text, assetimport, renderer (partly), lua, OpenAL, googletest, mbedtls would be shareable. Per-leg caches, not one shared cache.

## 3. Measures, savings, confidence

| # | Measure | Change size | Saves | Confidence |
|---|---|---|---|---|
| A | `concurrency:` with `cancel-in-progress` for pull_request runs only | 4 lines in tests.yml | Removes superseded runs (~5 of 40 runs, ~40 slot-jobs in 13 h); cuts queueing in bursts: contended runs 40-50 min -> ~30 min | High (waste removal), medium (queue minutes) |
| B | Skip the build legs for docs-only changes (a `changes` job + job-level `if:`) | ~25 lines | A docs PR goes from 8 x 10-28 min jobs per push to ~1 min; frees all 8 slots for other PRs | High |
| C | ccache on the 3 Linux legs (actions/cache, save on master only, PRs restore) | ~15 lines per leg, configure flag only | Linux Build 10-13 min -> 2-5 min warm; required check `linux (Release, x86_64, Clang)` 13.7 -> ~5-7 min; frees slots sooner. Does NOT shorten the run (Windows is critical path) | Medium-high (warm), cold run unchanged |
| D | Windows: Ninja Multi-Config + MSVC dev env + sccache (or ccache >= 4.6), `/Z7` for Debug | ~30 lines per leg, no CMakeLists change [M] | Release 26 -> ~6-10 min warm, Debug 11-13 -> ~4-6 min warm; Ninja alone (no cache) maybe 10-20 % [M]. Run wall 28 -> ~12-15 min warm | Medium (needs 2-3 trial runs) |
| E | Matrix policy: Windows Release (the critical path) only on master pushes / nightly / label; or drop Debug PhysX ON from PRs | a few lines | Immediately 28 -> ~16 min (Release off PRs) and -1 or -2 slot-jobs per run | High; **decision for user** (coverage) |
| F | "Require branches to be up to date" off (or keep it and accept the re-run) | repo setting | Removes the second full run per PR after each merge (the biggest single contributor to 60-80 min per change [M]) | High; **decision for user** (risk: semantic conflicts land on master and are caught by the master run instead) |
| G | Docker: run on PRs only when `deploy/**`, `CMakeLists.txt`, `cmake/**`, `editor-server/**` change (always on master); later ccache via a BuildKit cache mount | ~10 lines | ~10 min of one slot per unrelated PR run; not on critical path | High (slot saving) |
| H | PCH for `engine`, `engine_tests`, `editor_server_tests`, `editor_project_tests` (sol2, gtest, nlohmann, STL) | CMakeLists, ~10 lines per target | Cold builds 20-40 % [M]; complicates ccache (needs `sloppiness=pch_defines,time_macros`, clang `-Xclang -fno-pch-timestamp`) | Medium-low until measured |
| I | Unity build | configure flag | Large cold gain but test files' anonymous-namespace helpers and statics would collide [M]: not recommended (maybe for lua/mbedtls C only) | Low |
| J | OpenAL `GIT_SHALLOW TRUE` (or URL tarball + hash) | 1 line in CMakeLists | 15-40 s per Windows leg configure [M] | Medium |
| K | `ctest -j 2` | 1 flag | 10-30 s per leg; risk of port/file contention between test exes | Low value |
| L | Larger runners (8-16 core) | settings, **paid** | Roughly halves Build on cold runs [M] | Option only |

### Notes per measure
- **A**: `concurrency: { group: tests-${{ github.event.pull_request.number || github.ref }}, cancel-in-progress: ${{ github.event_name == 'pull_request' }} }` at workflow level. Master pushes are never cancelled (each merge keeps its own record). workflow_dispatch runs fall into the ref group of their branch; give them their own group if the orchestrator uses dispatch for ratification [M].
- **B**: a cheap `changes` job (git diff against the base, or `dorny/paths-filter` pinned by SHA) outputs `code=true` unless every changed file is under `docs/**` or `*.md` (and `.idea/`, `.vscode/`). The build jobs get `needs: changes` + `if: needs.changes.outputs.code == 'true'`. A job skipped by `if:` reports "skipped", which satisfies a required status check [M, GitHub docs], so the required `linux (Release, x86_64, Clang)` check does not block. Do **not** use `paths-ignore:` at the trigger: then the required check never reports and the PR is stuck. Master pushes always run everything.
- **C**: `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` on the configure line; `apt-get install ccache`; `CCACHE_DIR=${{ github.workspace }}/.ccache`, `ccache --max-size=500M`, `CCACHE_COMPRESS=1`; `actions/cache/restore` with key `ccache-<os>-<arch>-<compiler>-<sha>` and restore-keys prefix `ccache-<os>-<arch>-<compiler>-`; `actions/cache/save` only on `push` to master (PR runs can restore caches from the base branch [M]). Print `ccache -s` into `$GITHUB_STEP_SUMMARY` for the hit rate. Cache entries are immutable, hence the sha in the key.
- **D**: `ilammy/msvc-dev-cmd` (pinned) for the x64 environment, `-G "Ninja Multi-Config"` keeps `build/bin/<Config>/...` [M: multi-config generators append the config dir to CMAKE_RUNTIME_OUTPUT_DIRECTORY], which `PlaySmokeTest.ps1` (`build/bin/$Config/N2EditorHost.exe`, l.21) [V] and the smoke step need. `-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded` for the Debug legs (the linker still writes .pdb with /DEBUG [M]). `/MP` becomes a no-op under Ninja [M]. sccache via `mozilla-actions/sccache-action` with the GHA cache backend, or ccache with actions/cache like C. Cache size: Debug /Z7 objects are big; set 1-1.5 GB per Windows leg [M]. Vulkan SDK and PhysX caches unchanged.
- **E/F** are policy, not engineering; see decisions. E is the only measure that shortens the critical path with zero build risk.
- **Cache budget**: 10 GB per repository, LRU eviction [M]. Six legs x 0.5-1.5 GB per master generation fits if only master saves; per-PR saves would thrash and could evict the PhysX/Vulkan SDK caches (those re-download in under a minute, so eviction costs little) [M].

## 4. Staged plan (cheapest and safest first)

1. **Stage 1 (workflow only, no build change)**: A + B (+ G gating). One PR. Expected: no superseded runs, docs PRs ~1 min, fewer queue minutes in bursts. Run length of a code PR unchanged (~28 min uncontended).
2. **Stage 2**: C (ccache on linux x86_64 Clang, arm64, GCC). One PR; the first master run after merge seeds the cache, the next PR is the first warm measurement. Expected: Linux legs 10-14 -> 4-7 min, required check ~5-7 min.
3. **Stage 3 (decision-dependent)**: E and/or F as the user decides. With E (Release off PRs), PR wall 28 -> ~16 min immediately.
4. **Stage 4**: D on one Windows leg first (Debug OFF/TLS, the least critical), then the other two. Expected warm PR wall ~12-15 min with all 8 legs, ~8-10 min combined with E.
5. **Stage 5 (optional, after measuring)**: J (OpenAL shallow), H (PCH on engine/engine_tests, measured cold and warm), docker ccache via BuildKit cache mount, K.

Expected end state (warm caches, code PR, no contention): ~12-15 min wall with the full 8-leg matrix (from 28); required check ~5-7 min (from 14). Cold runs (cache miss, e.g. a change to a widely included engine header or the toolchain) stay near today's 28 min. Contended bursts shrink because jobs hold slots for minutes instead of 10-28 min and superseded runs are cancelled.

## 5. Risks
- ccache/sccache false hits: essentially none with direct/preprocessor mode [M]; a poisoned cache is fixed by bumping a key suffix. Release correctness is unchanged (same compiler, same flags).
- Ninja on Windows changes how the build is driven (environment from vcvars instead of MSBuild): the first runs may surface missing `/MP`-independent settings, the `-A x64` platform must come from the dev-cmd arch, and DLL post-build copies (PhysX, OpenAL) must still land next to the exes [M]. Do it on one leg first.
- `/Z7` makes Debug .obj files larger; link is unchanged; local developer builds are unaffected (only the CI configure line changes).
- Docs-only skip: a docs file that a test truly reads would slip; today none does [V grep], keep the filter narrow (`docs/**`, `**/*.md`).
- cancel-in-progress: a cancelled run shows red "cancelled" on the old head; the orchestrator must wait on the newest head's run (it should already).
- Dropping legs from PRs (E) moves failures to master (detected one merge later, needing a fix-forward PR).
- Cache budget (10 GB) eviction churn if PR branches also save.
- Actions in new steps must be pinned (by tag at least, ideally SHA), like the existing `@v6/@v7` usage.

## 6. Verification plan
- Baseline (already measured above): uncontended 28.4 min (run 38732431586), Windows Release Build 1576 s; contended 48.0 min with 19.5 min queue (run 38725839772).
- After each stage: one PR run, then one jobs-API request (`actions/runs/<id>/jobs`) to read per-step Build times and queue times (same script as this analysis). For C/D: measure twice, the cold run (first after merge) and a warm run (a trivial follow-up commit, e.g. a comment change in one .cpp), and record the `ccache -s` / `sccache --show-stats` hit rate from the job summary (no log download needed).
- Success criteria: Stage 1: no two runs for the same PR running at once; docs-only PR jobs skipped and the PR mergeable. Stage 2: warm Linux Build <= 5 min, hit rate >= 80 %. Stage 4: warm Windows Release Build <= 10 min, all ctest and smoke steps still green on all legs.
- Record results in the progress log.

## 7. Decisions for the user
1. **Which legs are REQUIRED to protect on every PR?** Today only `linux (Release, x86_64, Clang)` is a required check, but the orchestrator waits for all 8. Options: (a) all 8 on every PR (today); (b) PRs run Linux Clang + Windows Debug PhysX OFF/TLS + Windows Debug PhysX ON + protocol, and Windows Release / arm64 / GCC / docker run on master push + nightly (or via a `full-ci` label); (c) like (b) but keep Windows Release on PRs (it is the only Release+MSVC coverage) and drop Windows Debug PhysX ON from PRs. Recommendation: (c) if MSVC Release bugs matter most, (b) if speed matters most.
2. **Keep "Require branches to be up to date before merging"?** Off removes one full CI run per PR per intervening merge; the master push run then catches combination failures after merge.
3. **Docs-only skip (B)**: approve the path list `docs/**`, `**/*.md`, `.idea/**`, `.vscode/**`.
4. **Windows cache tool for D**: sccache (GHA cache backend, simpler on Windows) or ccache (one tool everywhere). Recommendation: ccache on Linux, sccache on Windows.
5. **Docker leg on PRs**: path-gated (recommended) or always.
6. **Larger (paid) runners**: only if the free measures are not enough.
