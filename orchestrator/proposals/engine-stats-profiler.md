# Proposal: host stats (EN5) for the Profiler panel (FE-S5)

Status: design only, no code. Written against engine master at 448b06e5 (editor protocol **1.26.0**) and frontend master at 4a8e91c. Tags: [V] verified in the tree at those commits, [M] from memory or general knowledge, not checked here, [E] estimate. `[FE]` marks a path in the frontend repo (N2Engine-Electron-Frontend); other paths are in N2Engine. Protocol: this proposal takes the next free minor, **1.28.0** (1.27.0 went to the PickResult normal, #297); `prefab-overrides.md` (EN4) then takes the next free minor.

Roadmap: `electron-ui-roadmap.md` section 14 ("Later (EN5): `GetEngineStats` ...") and the table rows EN5 (L, "after WebGPU G2+ settled") and FE-S5 (Profiler panel, M). CONTINUE.md queue item 6 says the WebGPU stages are settled, so the dependency is met.

## 1. Goals

1. The host reports **where a frame's time goes** (fixed ticks and physics, Update/LateUpdate, audio, render, frame readback) and the frame rate it reaches, on the **play host** (where the game runs) and, with less to say, on the edit host.
2. **Draw calls and triangles** of the last rendered frame, in all four backends (OpenGL, Vulkan, WebGPU, software).
3. **Counts and memory**: objects and components in the scene, Lua memory against its cap, physics bodies, and process memory where the platform gives it.
4. Cheap enough to leave on: no measurable cost when nobody asks, and a few microseconds per frame when someone does [E].
5. No effect on the simulation (the cross-leg state hash job must not move) and no log line per poll.

Out of scope for v1: GPU timings (timer queries in four backends), per-script or per-component timings, a capture-and-replay profiler, network RTT and loss (the Network panel and the B11 overlay own those).

## 2. What exists today [V]

- **Health only.** `GetEngineHealth` (command `0x50`, `editor-server/protocol/protocol.json:587-597`) answers subsystem states (`healthy`, `count`, `subsystems[]`), built from `Application::GetHealth()` (`engine/src/Application.cpp:934-`); the handler scrubs host paths for a client that does not see every event (`editor-server/src/EditorServer.cpp:6500-6514`). No counters or timers anywhere in the engine: a search for draw-call, frame or render statistics in `engine/`, `renderer/` and `editor-server/` finds none (the only "statistics" are VMA's in `renderer/src/vulkan/VulkanResources.cpp:1071-1072` and the netcode's `RpcStats`/`PredictionStats`/`TimeDilationStats` in `NetworkServer.hpp:269`, `NetworkClient.hpp:238-308`).
- **The frame.** `Application::Tick` (`Application.cpp:626-723`) is, in order: poll events, `Time::Update`/`Advance`, simulation clock, attach queue, the fixed ticks (`RunFixedTick` -> `PhysicsUpdate`: `ApplyPendingChanges`, `scene.FixedUpdate`, backend `Update`, `SyncTransforms`, `ProcessCollisionCallbacks`, 915-932) or a network session's `_fixedTickDriver`, pointer dispatch, `Update`, coroutines, `LateUpdate`, audio `AdvanceStream`/`Update`, `Render` if `options.render`, `ProcessDestroyed`, pending scene change. Clear sections, one function: the natural place for scoped timers.
- **The play host.** `EditorServer::RunGameFrame` calls `Tick` with `render = false` (`EditorServer.cpp:6343-6364`): game frames are **not drawn** unless the client asks (`RenderFrame` -> `Application::RenderGameFrame`, 5003-5007, then `ReadFrame` readback). `RunPlayFrame` runs one game frame then processes commands until a **16 ms** budget is spent (6366-6399; `DefaultPlayFrameBudget{16}`, `EditorServer.hpp:704-705`). So on the play host, "render time" is the cost of the client's frame requests, and the frame interval is at least the budget.
- **The edit host does not tick**: `Host.cpp:641-648` runs `RunPlayFrame` only in play mode, else `ProcessCommands(16 ms)`; an editor frame is `RenderEditorFrame` on request (`Application.cpp:986-1070`).
- **Lua memory**: `LuaRuntime::Instance().GetMemoryUsed()` (bytes the state holds, what the cap counts) and `GetLimits()` (`engine/include/engine/scripting/LuaRuntime.hpp:110-115`, `LuaRuntime.cpp:316-324`). One runtime state for scene scripts; editor extensions have their own states (edit host only).
- **Physics**: `IPhysicsBackend` (`engine/include/engine/physics/IPhysicsBackend.hpp`) has no statistics call; backends are PhysX and Jolt (`engine/include/engine/physics/{physx,jolt}`).
- **Process memory**: no platform query exists in `engine/src` (no `GetProcessMemoryInfo`, `getrusage` or `/proc/self`).
- **Renderer**: `IRenderer` (`renderer/include/renderer/common/Renderer.hpp`) has a precedent for state every backend applies at **record time**: the non-virtual `SetStageDim`/`WithStageDim` (247-255), which each backend's draw path calls. Each backend funnels draws through one place: the software renderer's `RecordDraw` (`renderer/src/software/SoftwareRenderer.cpp:1343-1365`, both `DrawMesh` overloads and `DrawObjects` go there), and `DrawMesh`/`DrawObjects` in `renderer/src/opengl/OpenGLRenderer.cpp:568-708`, `renderer/src/vulkan/VulkanDraw.cpp:1034-1047`, `renderer/src/webgpu/WebGpuDraw.cpp:671-685`; `BeginFrame` in each backend (OpenGL 201, Vulkan `VulkanRenderer.cpp:1239`, WebGPU `WebGpuRenderer.cpp:1040`, software 1090).
- **Polled commands never log**: `IsPolledCommand` (`EditorServer.cpp:4441-`) lists `GetPlayState`, `PollEvents`, `RenderFrame` and others.
- **Frontend**: FE15 client-side stats exist: `[FE] src/renderer/client-stats.ts` (rolling `Series`, 5 s window `ClientStatsWindowMs`, refresh `ClientStatsRefreshMs = 250`, request latency, pixel bytes/s, displayed fps, event poll lag, play frames and game time per second) shown by `[FE] src/renderer/client-stats-panel.tsx` inside the Engine panel (`[FE] src/renderer/panels.tsx:61-74`). Panels are registered through `[FE] src/renderer/layout/panel-registry.ts` and `builtin-panels.tsx`. The play host is a second connection the launcher opens (`WritePlaySnapshot` description, protocol.json ~884), so a Profiler polls whichever host is active.

## 3. What the host measures

### 3.1 Frame timers (engine)

`engine/include/engine/diagnostics/FrameStats.hpp` (new): a `FrameStats` singleton owned by `Application`, with a fixed **ring of the last 240 frames** [E: 4 s at 60 fps] of per-frame records, and a `ScopedSection` RAII timer on `std::chrono::steady_clock`.

Per frame record (all milliseconds as `float`, counts as `uint32`):

| Field | Measured around |
|---|---|
| `frame` | the play host's frame counter (`_playFrame`), or `Tick` count elsewhere |
| `intervalMs` | wall time since the previous frame's start (the frame rate) |
| `tickMs` | the whole `Tick` |
| `fixedMs`, `fixedTicks` | the fixed-tick loop (or the `_fixedTickDriver` call), and how many ticks ran |
| `physicsMs` | backend `Update` + `SyncTransforms` + `ProcessCollisionCallbacks` inside those ticks (the rest of `fixedMs` is `FixedUpdate` and attach) |
| `updateMs` | pointer dispatch + `Update` + coroutines + `LateUpdate` |
| `audioMs` | `AdvanceStream` + `Update` |
| `renderMs` | `Render` when the tick renders (games and n2server never ask the editor) |
| `commandsMs` | play host only: time spent handling commands in this frame's budget, `renderRequestMs` and `readbackMs` (the client's `RenderFrame`: `RenderGameFrame` and `ReadFrame`) split out of it |

Timers read the clock only; nothing they record feeds the simulation, so determinism is untouched (the clock is already read every frame by `Time::Update`). The ring is a fixed array: no allocation per frame.

Gate: `FrameStats::SetEnabled(bool)`. Off (default) costs one branch per section. The host turns it on at the first `GetEngineStats` and off again when nobody has asked for 10 s [E], so an unwatched host pays nothing.

### 3.2 Render counters (renderer, four backends)

`renderer/include/renderer/common/RenderCounters.hpp` (new): `{drawCalls, triangles, vertices, frameWidth, frameHeight}`. On `IRenderer`, non-virtual like the stage dim: `NoteDraw(indexCount, vertexCount)`, `ResetCounters()` and `GetLastFrameCounters()`. Each backend calls `NoteDraw` where it **records** a draw (the same spot it calls `WithStageDim`: software `RecordDraw`, the `DrawMesh`/`DrawObjects` bodies of the other three) and `ResetCounters` in `BeginFrame`; `EndFrame` publishes the frame's counters as "last frame". Triangles = indices / 3 for triangle lists (all built-in pipelines draw triangle lists [M: verify per backend while implementing]); a wireframe draw counts the same. A backend (or test fake) that never calls them reports zeros, and the stats say `renderCounters: false` for it via a virtual `SupportsRenderCounters()` (default false; the four override true), mirroring `SupportsStageDim()` (`Renderer.hpp:256-258`).

Cost: two integer adds per draw.

### 3.3 Snapshots taken at request time (no per-frame cost)

- **Scene**: objects, active objects, components, Lua components, renderables: one `Scene::TraverseAll` per request (linear; at 4 Hz and the 10,000-object prefab cap this is negligible [E]). Counts only, no names.
- **Lua**: `memoryBytes` (`GetMemoryUsed`), `memoryCapBytes` (`GetLimits`).
- **Physics**: `IPhysicsBackend::GetStats()` (new virtual, default empty) returning optional `{bodies, activeBodies, staticBodies, contacts?}`. PhysX from `PxScene::getSimulationStatistics` and Jolt from `PhysicsSystem::GetNumBodies`/`GetNumActiveBodies` [M: both APIs from memory; confirm at implementation]. Fields a backend cannot give are absent, not zero.
- **Process**: `workingSetBytes` / `peakWorkingSetBytes` from a small platform function (`GetProcessMemoryInfo` on Windows, `/proc/self/statm` on Linux, the wasm heap size under Emscripten [M]); absent where unsupported.
- **Host**: renderer name, whether stats are enabled, the play state (Edit/Playing/Paused), the time scale.

## 4. Protocol (1.28.0)

Hello capability `engineStats`.

| Command | Class | Meaning |
|---|---|---|
| `GetEngineStats {sinceFrame}` | Read | Answers `EngineStats` (JSON). Turns the frame timers on if they were off (the first answer then has no frames yet). `sinceFrame`: the client's last seen `frame`; the answer's `frames` holds every ring record after it (at most 240), so a client polling at 4 Hz gets every frame without polling at 60 Hz. `0`: the whole ring. Listed in `IsPolledCommand` (never logs). |

`EngineStats` (`json:EngineStats`, the newer commands' style, like `PrefabStageInfo`):

```json
{
  "host": {"kind": "play", "state": "Playing", "renderer": "OpenGL", "timersEnabled": true, "timeScale": 1},
  "frame": 18234,
  "frames": [ {"frame": 18231, "intervalMs": 16.7, "tickMs": 2.1, "fixedMs": 0.9, "fixedTicks": 1, "physicsMs": 0.6,
               "updateMs": 0.8, "audioMs": 0.1, "renderMs": 0, "commandsMs": 4.2, "renderRequestMs": 3.1, "readbackMs": 0.8} ],
  "summary": {"windowFrames": 240, "fps": 59.8, "tickMs": {"avg": 2.0, "p95": 3.4, "max": 9.1}},
  "render": {"counters": true, "drawCalls": 412, "triangles": 183220, "vertices": 120004, "width": 1280, "height": 720,
             "lastRenderedFrame": 18233},
  "scene": {"objects": 1530, "activeObjects": 1490, "components": 4120, "luaComponents": 61},
  "lua": {"memoryBytes": 3145728, "memoryCapBytes": 268435456},
  "physics": {"backend": "physx", "bodies": 820, "activeBodies": 37},
  "process": {"workingSetBytes": 412000000}
}
```

Absent blocks mean "not available here" (no physics backend, no platform memory query). Command and response ids from the free ones at implementation (now: commands `0x2F`, `0x42`-`0x4F`, `0x54`-`0x5F`; responses `0x27` onwards [V `protocol.json` id lists]).

**Why poll, not push** (decision 1): every frontend view of host state is pulled (`GetPlayState` for the toolbar, `PollEvents` for events); a pushed stats event at frame rate would flood the event queue the console and hierarchy depend on, and a pull costs nothing when the panel is closed. `sinceFrame` removes the only reason to push (missing frames).

**Role**: Read; a viewer may call it (the numbers name no file or object). The edit host answers too (timers only for the render requests it serves; `frames` empty when nothing ticks). Not offered to Lua extensions. The MCP bridge can expose it as a read tool later (frontend repo).

## 5. Cost [E]

- Timers off: one predictable branch per section, about 10 per frame.
- Timers on: about 12 `steady_clock::now()` calls per frame (tens of nanoseconds each on desktop [M]) and a 240-entry ring write: well under 10 microseconds per frame.
- Render counters: two adds per draw, always on (cheaper than a branch to skip them).
- A request: one JSON object of at most 240 frame records (about 40 KB worst case) and one scene traversal; at 4 Hz that is around 160 KB/s on loopback, small next to the frame images the viewport already pulls.
- Web build: `steady_clock` maps to `performance.now()`, whose resolution browsers coarsen [M]; the web player has no editor protocol, so this only matters for a later in-game overlay.

Engine size **L** [E ~1200-1500 lines with tests and docs; most of it the four backends' counters and tests]; frontend FE-S5 **M** [E ~800].

## 6. Frontend: the Profiler panel (FE-S5, outline)

- A registry panel "Profiler" (`[FE] layout/panel-registry.ts`), shown when Hello lists `engineStats`; otherwise a one-line "this host has no engine stats (older engine)".
- Polls `GetEngineStats` every 250 ms (the existing `ClientStatsRefreshMs`) on the **active** connection: the play host while playing, else the edit host; only while the panel is visible; latest-wins (one request in flight). Keeps `sinceFrame` and appends records to a client ring (e.g. 1200 frames, about 20 s at 60 fps); resets on reconnect or when `frame` goes backwards (new play session).
- **Frame-time graph**: a sparkline/area of `intervalMs` with 16.7 ms and 33.3 ms guide lines, and a stacked bar per frame of `fixedMs`/`updateMs`/`audioMs`/`renderMs`/`commandsMs` under it; hover shows the record. Pause button freezes the view, not the polling.
- **Counters table**: draw calls, triangles, objects, components, Lua memory (bar against the cap, warning colour above 80%), physics bodies (active/total), process memory; "—" for absent values, as the client stats panel does.
- The existing client stats stay in the Engine panel; the Profiler links to them (they measure the client side of the same loop).
- Tests: Node for the merge of polled records (`sinceFrame` gaps, overlap, reset on a new session, ring cap), summary maths; DOM for the panel with a fake engine (capability off, absent blocks, warning colour); a live check in the app is the user's.

## 7. Test plan (engine)

- `FrameStats` unit tests: sections nest and sum, the ring wraps at 240, `sinceFrame` returns exactly the newer records, disabled records nothing, re-enabling starts clean.
- Renderer: the software renderer's counters for a known draw list (N `DrawMesh` calls of known meshes give N draws and the right triangles; an `IndexRange` counts its range; `DrawObjects` counts each); counters reset at `BeginFrame` and publish at `EndFrame`; the reorder in the software backend does not change counts. OpenGL, Vulkan and WebGPU: the same test where CI has the backend (OpenGL headless legs; Vulkan and WebGPU where their jobs run), otherwise a record-path test; a fake renderer reports `counters: false`.
- Host (`tests/editor-server`, next to `EngineHealthProtocolTests.cpp`): `GetEngineStats` on an edit host (no frames, scene counts right); on a play host after `Step(5)` exactly five records with increasing `frame`; `sinceFrame` semantics over the wire; a `RenderFrame` fills `render` and `renderRequestMs`/`readbackMs`; a viewer may call it; it never logs (the polled-command test); timers switch off after the idle timeout (with an injected clock); protocol golden, test vectors and generated clients at 1.28.0; Hello capability.
- Determinism: the cross-leg state hash job and the lockstep/rollback tests unchanged with timers on (run one with timers forced on).
- Docs in the same PR (project rule): `docs/editor.html` (stats command and the Profiler), `docs/application.html` (the frame sections), protocol.json.

## 8. Risks

1. **Four backends again**: the counters touch OpenGL, Vulkan, WebGPU and software, the same friction as the stage dim; small per backend (two calls), but each needs its test.
2. **Misread render numbers on the play host**: the game is drawn only when the client asks, so `renderMs` and draw calls describe the editor's frame requests, not a shipped game's loop. The panel labels it ("as drawn for the editor viewport").
3. **Physics API facts are from memory** [M]; a backend may give fewer fields, which the optional fields allow.
4. **Process memory differs per platform** (working set vs RSS); shown with the platform's name for it.
5. **No GPU time**: a GPU-bound game looks idle on the CPU side; decision 3.

## 9. PR order

1. Engine + protocol 1.28.0: `FrameStats` and sections in `Application::Tick` and the play host, render counters in the four backends, `IPhysicsBackend::GetStats` for PhysX and Jolt, process memory, `GetEngineStats`, docs, tests.
2. Frontend FE-S5: protocol sync, Profiler panel.
3. Docs-only ledger and MCP entries.

## DECISIONS for the user

1. **Poll or push**: `GetEngineStats {sinceFrame}` polled at 4 Hz by the panel, over a pushed stats event. Recommendation: poll.
2. **Per-frame history on the host**: a 240-frame ring returned incrementally (every frame reaches the graph), over aggregates only. Recommendation: the ring.
3. **GPU timings**: not in v1 (CPU-side sections only); add timer queries per backend later if a GPU-bound case needs them. Recommendation: later.
4. **Protocol number**: 1.28.0 (1.27.0 was taken by #297), before EN4 overrides. Decided at implementation.
