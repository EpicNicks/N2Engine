# #424: what a threaded web artifact would buy, and how to measure it first

Issue #424 (stage 6 of #155) asks for the measurement write-up before any threaded build: what threading would buy, what it
costs, and what to measure. No threaded build was made for this note (the goal said so); every statement below is read from
the code on master (2026-11-22) or quoted from a measurement that exists.

## What `N2ENGINE_SINGLE_THREADED` turns off today

`cmake/N2EngineWeb.cmake` defines it for every web target. It gates exactly four things:

| Where | Single-threaded (web today) | Threaded (native) | What a threaded web build could gain |
|---|---|---|---|
| `renderer/include/renderer/software/RenderThread.hpp` | a submitted frame runs at once on the calling thread | one render thread runs the frame's commands while the next frame's logic runs (a pipeline of two, not row-parallel rasterisation) | overlap of game logic and software rasterisation: at most the shorter of the two per frame. Only the software renderer: WebGPU already hands its work to the browser's GPU process |
| `engine/src/physics/physx/PhysXBackend.cpp:637` | the CPU dispatcher has 0 workers | 4 workers, or 0 under `physics.deterministic` | PhysX's island solver on workers, for a game that does NOT use the Deterministic preset (the preset forces 0 everywhere) |
| `engine/src/physics/jolt/JoltBackend.cpp:3540` (and `:47`, `:3589`) | the single-threaded job system; `DeclaredDeterminism` says single-threaded | the thread pool unless deterministic | Jolt's job system on workers, again only outside the Deterministic preset; `PhysicsCorpusThreadsTest` (#435) shows the state does not depend on the worker count, natively |
| `engine/src/scripting/ScriptDeclarationCache.cpp` | declarations evaluated on the calling thread | a worker with a deadline | nothing for a player: the cache is the editor host's |

So threading would help a web game only where (a) it draws with the software renderer, or (b) it runs 3D physics outside
the Deterministic preset with enough bodies that the step matters. A networked deterministic game gains nothing: the preset
runs physics single-threaded on every platform.

## What it would cost

- **Hosting:** a threaded artifact needs `SharedArrayBuffer`, so the page must be cross-origin isolated (`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`, or `credentialless`). Static hosts that cannot set headers (and embeds in other sites' iframes) cannot serve it. That is why the single-threaded baseline with no special headers stays the decision; a threaded artifact can only ever be a second one the loader picks when `crossOriginIsolated` is true.
- **A second artifact:** `index.wasm` and `index.js` built with `-pthread` (and `-sPTHREAD_POOL_SIZE`), plus the worker script; the package smoke's "no worker script" rule and the budgets get a second profile row.
- **PhysX:** the web PhysX libraries (`scripts/web-build-physx.sh`) are built without `-pthread`, and neither their `.pin` stamp nor the CI cache key (`physx-web-emsdk-...`) records threading, so a threaded artifact needs its own PhysX build and cache entry (web-build.html already says so).
- **WebGPU and the main thread:** the WebGPU backend and the canvas stay on the main thread; Emscripten proxies calls from workers, so only the render-thread pipeline above (software) and the physics workers move work off it.
- **CI time:** a third full web build (the web job is the long pole even after the per-profile matrix of #437).

## What to measure before deciding (none of it has been measured)

1. **Where a web frame's time goes today**, in a real browser on the size profile: the engine's own frame stats (`GetEngineStats` timings, or `performance.now()` around the main loop), split into logic, physics step and render submission, for `lua_project` and for a 400-body physics scene. If physics or software rendering is not most of the frame, threading cannot pay for its hosting cost.
2. **Physics step cost under node**, which CI now prints: the `Physics bench numbers` step of the web dev leg (#435) gives Jolt and PhysX single-threaded step medians for 25/100/400 bodies under node (V8, single-threaded). Compare with the native legs' numbers in the same run to see how much slower wasm is; a threaded web build could at best divide the step by the worker count on the scenes that keep many islands awake (the `active` and `joints` scenes of the bench).
3. **Native thread-pool scaling** of the same bench (not printed today: the bench runs single-threaded only). Run `PhysicsBackendBenchTest` with the thread pool (`SetDeterministic(false)` plus `SetWorkerThreadsForTesting`) at 1, 2 and 4 workers natively; the ratio is an upper bound for what a threaded wasm build could gain.
4. **Software renderer frame time with and without the render thread**, natively (the pipeline exists there): the same scene with `RenderThread` and with the single-threaded fallback, to bound the overlap.
5. **The size of a `-pthread` build** of the size profile (`index.wasm` raw/gzip/brotli against the current 2.1 MB brotli), and its start-up time in the Chromium gate (`[WEB-TTFF]`, #438) with the pthread pool created.

## Recommendation

Do not build the threaded artifact until 1 and 3 show that physics (or software rendering, 4) is a large share of a web frame
for a game that is not on the Deterministic preset. Items 2 and 5 are cheap to get from CI once someone wants the answer:
2 is already printed; 5 needs one extra configure in the size leg behind a flag. Keep the single-threaded, header-free
artifact the default whatever the result.
