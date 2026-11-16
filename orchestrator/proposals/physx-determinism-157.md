# #157 determinism audit refresh: blockers at master, PhysX vs Jolt rollback, divergence experiment, project-setting design

Status: research plus one new test file; no engine source changed, nothing committed. N2Engine `master` at `2b3b3a74` (2026-11-14), working tree clean before this file and the test below.
Scope: the determinism parts of #157 only (audit, staged plan, rollback needs, divergence experiment, network-simulation-model setting). The limitations table is `physx-limitations-157.md` (another agent) and the cost of a custom engine is the #156 report; not repeated here.
Marks: **[V]** verified in this tree (path:line) or run here, **[M]** memory / general knowledge, unverified, **[E]** my estimate.

## 0. The important finding: most of this already exists

`proposals/physx-limitations-and-determinism.md` (the first #157 pass, written at `7222391`, 2026-10-15) and `proposals/netcode-runtime-design.md` (`637797c`, 2026-10-20) predate about 270 files of work. Reading them against `master` shows the design is **built, not proposed**:

| Design item (first #157 pass / netcode design) | State at `2b3b3a74` [V] |
|---|---|
| Three presets none / authoritative / deterministic, settings block `network` | Done: `engine/include/engine/network/NetworkSettings.hpp`, `NetworkPresets.hpp` (26-row preset table, L/T/S classes), docs/physics.html "network" section |
| Startup validation, refuse to start (D4) | Done: `NetworkValidation.hpp` (`ValidateNetwork`, requirements `physics-backend, physics-features, strict-fp, simd-scalar, lua-seed, engine-trig, physics-threads, gpu-physics-off, fixed-timestep, tick-loop`, plus `snapshot` in rollback mode); called at `engine/src/Application.cpp:255` and `:583`; refusal leaves seven NotStarted rows and a Failed Network row (docs/physics.html "network-refusal") |
| Editor Network panel, nudge, picker | Host side done: `tests/editor-server/project/EditorNetworkPanelTests.cpp`, `EditorPhysicsBackendTests.cpp`, protocol `GetPhysicsBackends/SetPhysicsBackend` (docs/physics.html "Editor picker", protocol 1.20.0) |
| B1 SIMD tier | `math.simdTier` setting read at `Application.cpp:345` (`Math::InitializeSIMD(simdRequest)`); deterministic preset requires Scalar |
| B2 strict FP | `N2ENGINE_STRICT_FP` (`CMakeLists.txt:92-123`), `N2_STRICT_FP`, `Math::IsStrictFp()`; default ON only for web size/speed profiles (`:87-91`) |
| B3 unordered iteration | Coroutines and touch maps ordered (`ContactTracker.hpp:95` comment, std::map) |
| B4 Lua | `determinism.luaSeed` (`Application.cpp:315`), `Ordered.pairs` and engine-trig `math.*` in `engine/src/scripting/LuaDeterministic.cpp:33-156`, session lint |
| B5 fixed step | `SimulationClock` owns the tick loop in network modes; `Time::SetFixedIgnoresTimeScale(IsNetworked())` at `Application.cpp:723`; attach queue per tick (`:754`) |
| B6 RNG | `Random.Seed` -> `DeterministicRandom` (`math/include/math/Random.hpp:12-43`) |
| B8 libm | `math/include/math/DeterministicMath.hpp` (Sin/Cos/Atan2/...); `tests/engine/DeterministicMathSourceScanTests.cpp` fails on any new `std::sin` etc. outside a 3-file audited list (Camera, Mesh, ParticleEmitter: drawing/cosmetic) |
| B10 backend | **Jolt backend exists**: `engine/src/physics/jolt/JoltBackend.cpp`, `cmake/jolt/CMakeLists.txt`, option `N2ENGINE_USE_JOLT` (default OFF, `CMakeLists.txt:34`) |
| Rollback | `RollbackWorld`, `RollbackSession`, `RollbackRuntime`, `LockstepRuntime/Relay`, `StateHash`, spectators and late join from snapshot (docs/netcode.html "rollback") |

So this report is an audit of **what is still a blocker**, the rollback comparison, the experiment, and a short list of gaps in the setting design. I align with the existing design and do not re-propose it.

## 1. Blocker inventory at `2b3b3a74`

Legend: R = remaining blocker for the Deterministic preset, P = partial, D = done (listed so nobody re-audits it).

| # | Area | State | Evidence [V] | What is left / stage |
|---|---|---|---|---|
| 1 | **Physics backend** | R for PhysX, P for Jolt | PhysX declares at most `SameBinary` (`PhysXBackend.hpp:99-103`, `DeclaredDeterminism(_deterministic, workers)`); Jolt declares `CrossPlatform` only with single-threaded job system + `JPH_CROSS_PLATFORM_DETERMINISTIC` + `N2_STRICT_FP` (`JoltBackend.cpp:2921-2937`). Jolt is in no local build (`build/CMakeCache.txt`: `N2ENGINE_USE_JOLT=OFF`) | S-A: make Jolt the Deterministic backend (done in code); S-B: promote the J7 CI corpus to a required check (docs/physics.html "jolt-determinism-ci" says it becomes required after two green weeks); S-C: prove the thread-pool path or keep the single-threaded job system forever (`JoltBackend.cpp:2971` comment: declaration deliberately conservative) |
| 2 | **FP contraction / fast-math** | P | No `-ffast-math`, `/fp:fast` anywhere in the tree; strict profile forbids them and `-march=native` (`CMakeLists.txt:96-107`); contraction off in Jolt always (`cmake/jolt/CMakeLists.txt:92-107`) and in math on aarch64 (`math/CMakeLists.txt:59`) and for `DeterministicMath.cpp` per file (`:68-74`). **Default for native Windows/Linux builds is `N2ENGINE_STRICT_FP=OFF`** (`CMakeLists.txt:87`). x86 builds enable `-mavx` without `-mfma`, so GCC/Clang cannot contract today (consequence, not a guard) | R: deterministic projects must ship from a strict build; this is already a refused requirement (`strict-fp`), so the failure is loud. Optional: default strict ON for any build that includes Jolt. PhysX (prebuilt NVIDIA MSVC binaries on Windows, our CI-built static libs on Linux, `CMakeLists.txt:197-209`, `cmake/PhysX*Hashes.cmake`) has its own flags we do not control |
| 3 | **SIMD tier summation order** | P | Tier is chosen at runtime from CPU detection unless `math.simdTier` is set; tiers are only tested to a tolerance (`tests/math/math_types/SimdDispatchTests.cpp:52`, `ExpectClose`). Quaternion dot: scalar sums left to right (`math/include/math/Quaternion.hpp:229`), SSE2 pairwise (`:283`), SSE4.1 `_mm_dp_ps` (`:364`) | D by forcing Scalar (requirement `simd-scalar`). R (optional, perf): bit-exact tiers verified by exact-equality tests so a deterministic project can use SIMD; low value because Jolt and the sim glue are the hot path, not `Quaternion::Dot` |
| 4 | **Unordered-container iteration in physics/scene** | D (sim paths), residual lookups | `PhysXBackend.hpp:429,474,482,509,510` are `unordered_*` but only looked up / `contains` / one-key (`_pendingChanges` has the single key `SetGravity`, `:477-480`, iterated at `PhysXBackend.cpp:884`). `GameObject.hpp:67` `_componentMap` is lookup only. `InputSystem.hpp:37`, `ActionMap.hpp:143` string-keyed unordered maps: sim must read the sorted `InputFrame`, not the maps (netcode design 4.3). `ReplicationLayout.hpp:81,99` lookups | Cheap guard: extend the source-scan test idea of `DeterministicMathSourceScanTests` to flag `for (... : <unordered container>)` in `engine/src/physics`, `engine/src/network`, `engine/src/Scene*` with an audited-list. Nothing found iterating today |
| 5 | **Fixed-step loop** | D in network modes; residual in None | In None mode the clock still drops time past 0.25 s and scales dt by `timeScale` (`Application.cpp:718-723` comment). `Time::GetTime()` is a float of accumulated real frame deltas (`engine/src/Time.cpp:58-61`) | R: any sim script that reads `Time.time` (not tick*dt) in a deterministic project is wrong; add the `sim-reads-wallclock` lint (the session lint already names module-locals and non-replicated writes; see docs/netcode.html "rollback-contract") |
| 6 | **RNG** | D | `Random.Seed` switches to `DeterministicRandom`; the unseeded default is `std::mt19937` from `std::random_device` (`Random.hpp:20-21`) | Deterministic preset refuses unseeded sim use (netcode design B6). UUIDs stay random per thread (`math/src/math/UUID.cpp:89-94`) so any sim logic keyed on a UUID is non-deterministic: net ids replace them (B9) |
| 7 | **Threading** | P | PhysX dispatcher 4 workers by default, 0 with `physics.deterministic` (`PhysXBackend.cpp:641-645`); Jolt single-threaded job system when deterministic (`JoltBackend.cpp:2971-2990`). Software renderer `RenderThread` (`renderer/include/renderer/software/RenderThread.hpp:17`) only draws. Logger buffers per thread id (`Logger.hpp:93`), no sim effect. `std::thread` appears only in `LockstepRuntime.cpp:145` (a network pump) and `ScriptDeclarationCache.cpp:379` (declaration scan workers); neither writes sim state [V grep] | PhysX thread-count independence is **measured here** (section 3: 0 vs 4 workers bit-identical in all scenes) but not promised by the vendor |
| 8 | **libm / transcendentals** | D | `DeterministicMath.hpp` pins exact bit patterns in `tests/math/math_types/DeterministicMathTests.cpp` [per the header comment `:14`]; scan test guards regressions; Lua `math.*` redirected (`LuaDeterministic.cpp:33-86`) | Residual: `std::sqrt` is IEEE-exact so fine; PhysX and Jolt internals have their own `sin/cos` (Jolt avoids libm under CROSS_PLATFORM_DETERMINISTIC [M]; PhysX does not promise) |
| 9 | **Denormals / FTZ / DAZ, NaN payloads** | P | `math/include/math/FloatEnvironment.hpp:30,83` (a float-environment guard exists) | Verify the guard is held on every thread that steps physics, and that hashes canonicalise NaN (netcode design B13, `StateHash`). Third-party code (audio, GPU drivers) that sets FTZ in the main thread would change results; unmeasured |
| 10 | **WASM vs native** | P | PhysX web: CPU only, 0 threads, "results are not bit-identical to Windows or Linux" (docs/physics.html line 130). Jolt web builds scalar, `USE_WASM_SIMD OFF`, `JPH_USE_WASM64 OFF` (`cmake/jolt/CMakeLists.txt:55-67`); web size/speed profiles default strict FP (`CMakeLists.txt:82-91`); the web-node leg is one of the five legs of the J7 corpus | D for Jolt (strict + scalar + wasm has no FMA), assuming the browser engine honours IEEE basic ops (spec: yes; NaN payloads and relaxed-SIMD excepted [M]). Never enable relaxed SIMD in a deterministic build. PhysX on the web is not a deterministic option |
| 11 | **Debug vs Release, per-leg builds** | R (found here) | Windows Debug legs are built without strict FP (docs/physics.html "jolt-determinism"), so they declare `SameBinary` at best. My local Debug PhysX runs match CI Linux Release for free fall but not CI Windows Release (section 3.3), so even the same vendor DLL family gives different bits across configs | Policy: peers of a deterministic session must be Release (or strict) builds of the same toolchain class; the join fingerprint (`NetworkFingerprint.hpp`, "world-fingerprint") should carry compiler, arch, strict flag and `N2_STRICT_FP`/Jolt-cross-platform so a Debug peer is refused at join rather than detected as a desync later (verify it already does; I did not read the fingerprint fields) |
| 12 | **Lua state outside replicated fields** | R (documented limit) | docs/netcode.html "Limits that remain": locals, closures, coroutines are not in a snapshot | Real, inherent; the desync detector is the backstop. Keep the lint; later: a restricted sim-Lua profile (no closures over sim state) |
| 13 | **Scene data parsing/printing** | P [M] | nlohmann::json float parse/print | Covered by the cross-leg scene-hash test per netcode design B14; not re-verified |

Net: for **Jolt + strict build + Scalar + single-threaded**, the engine has no known blocker left in code; what remains is proof breadth (corpus scenes, thread pool, Debug, other CPUs), the sim-Lua boundary and policy (Release-only peers). **PhysX can never satisfy the preset** (different binaries per OS, no promise across binaries) and is kept for the authoritative preset.

### Staged plan (remaining work only)

1. **Cheap, now:** default `N2ENGINE_STRICT_FP` ON whenever `N2ENGINE_USE_JOLT` is ON (one line, removes the commonest refusal); add the unordered-iteration scan test; wall-clock lint for sim scripts; confirm the join fingerprint includes strict/cross-platform flags (blocker 11).
2. **Medium:** extend the J7 corpus with a joint/ragdoll scene (Jolt constraints are unused by the backend today: `JoltSnapshotTests` says "constraints (the backend uses none)" [docs/physics.html jolt-snapshot-contents]; the engine has **no joint component for either backend**, so a ragdoll in-engine does not exist yet, which is why my ragdoll test below uses the raw PhysX API); add a Release-vs-Debug leg to find what Debug changes; run the thread-pool path through the corpus and decide whether to ever declare it.
3. **Large / optional:** bit-exact SIMD tiers (blocker 3); second-CPU-family legs (AMD vs Intel, Apple M) because the runners do not cover them (docs/physics.html "What a green run shows").

## 2. Rollback: what a physics step reads and writes, and snapshot cost

**What a step reads/writes [V from the backends and docs]:**
- Reads: body poses, velocities, mass/inertia, sleep state and timers, **contact cache and warm-start impulses** (solver state that persists across steps), constraint state, collision filter data and layer matrix, gravity, dt, kinematic targets, the order bodies were inserted, pending (not yet applied) changes (`PhysXBackend.hpp:477` `SetGravity`).
- Writes: all of the above, plus contact events (`ContactTracker`), trigger overlaps and the engine's transform sync (`SyncTransforms`).
- Rollback state therefore = body state + solver caches + engine-side bookkeeping (tracker, queues, matrix, gravity) + RNG + net-id set + sim-Lua replicated fields (`RollbackWorld.hpp`, docs/netcode.html "rollback-snapshot").

**PhysX:** no scene save/restore in the API; `SupportsSnapshot()` is false by declaration (`PhysXBackend.hpp:105`, `kSupportsSnapshot = false`), `SupportsSetState()` true (`:106`). The only restore is writing each body's pose, velocities and sleep state back (the `SetBodyState` route). PhysX's serialization (binary/RepX) rebuilds actors and is for loading content, millisecond-class [E, not measured here] and still does not carry contact caches [M].
**Measured here** (new test, Debug, raw PhysX, 60 mixed bodies, one sample; section 3.4): capture 75.6 us, write-back 319.0 us. But restoring step 150 and re-running 150 steps **did not reproduce** the straight run: max position difference 0.99 m. Cause: warm-start impulses and contact caches are lost on restore. So set-state is enough for *authoritative reconciliation* (a few predicted bodies corrected, chaos absorbed by the server) and **not** for rollback resimulation.

**Jolt:** `SaveState/RestoreState` through `PhysicsSystem::SaveState(EStateRecorderState::All)` (`JoltBackend.cpp:2666`, `:3793`) captures bodies, contact cache and constraints, plus the engine's per-body slots, matrix, gravity, tracker and event queues, with magic/version/FNV checksum framing (docs/physics.html "jolt-snapshot-contents"). Tested: restore gives the saved state and the same future; a state restored into another backend gives the same future; rewinding K ticks with a corrected input equals a straight run with it; 0-vs-N worker threads give the same state (`tests/engine/physics/JoltSnapshotTests.cpp:236,261,278,543,586`); corpus `rollback` and `snapshot_roundtrip` scenes are in the cross-leg compare. One documented inexactness: restoring across a collider change rebuilds the shape and invalidates that body's contact cache, so the replay may differ in the last bits (docs/physics.html jolt-snapshot-contents).
**Cost: not measured in the tree.** docs/netcode.html "rollback-measurement" says so explicitly. Jolt was not built locally here (OFF in the cache; the stale build is 272 files behind), so I could not measure it. The measurement spec in that section is the right one; the thing it lacks is a committed microbenchmark (save/restore time and bytes for 100 / 1000 bodies) that runs in CI next to the corpus.

**Comparison for rollback:** PhysX: no usable rollback (infidelity above, `SupportsSnapshot` false). Jolt: designed for it (state recorder includes the caches), cross-platform declared, tests green. A plain-data custom engine would only beat Jolt on snapshot size and trivially copyable state; that argument belongs in #156 and does not change what to ship.

## 3. Divergence experiment (part b)

### 3.1 What was already there and what I ran
`tests/engine/physics/PhysXDeterminismTests.cpp` (PR #161) already implements the +1 ulp experiment for free fall, a 10-box stack and a 100-body pile with the engine's `PhysXBackend`, printing `[DETERMINISM]` lines; CI numbers are in `proposals/physx-determinism-measured-numbers.md`. There is **no ragdoll** scene, and the backend has no joints, so I added `tests/engine/physics/PhysXRagdollDeterminismTests.cpp` (section 3.4).

**Build situation:** `build/` exists (VS, RelWithDebInfo cache, `N2ENGINE_USE_PHYSX=ON`, `N2ENGINE_USE_JOLT=OFF`, `N2ENGINE_STRICT_FP=OFF`) with `build/bin/Debug/engine_tests.exe` dated 2026-11-08, six days and ~270 files behind `master`. I did **not** rebuild (large). The stale exe already contains `PhysXDeterminism*`, so I ran it directly. The new test file was compiled and run **standalone** (cl 19 / VS 2022, `/MTd`, the repo's PhysX 5.6.1 Debug import libs and the build's gtest) because it needs only raw PhysX and gtest; it is **not yet compiled through the in-tree build** (tests/engine/CMakeLists.txt uses `file(GLOB_RECURSE ...)`, so it is picked up at the next reconfigure; it compiles to a skip when `N2ENGINE_PHYSX_ENABLED` is undefined; on Linux the static `PHYSX_LIBS` list must supply PhysXExtensions, which is in the root list but I did not build there). Jolt could not be run at all (not in any build).

### 3.2 Same-binary repeatability (asserted + cross-process)
Environment: Windows 10, Debug `engine_tests.exe`, PhysX Debug DLLs, enhanced determinism on, 0 workers, dt 1/60, 600 steps.
- In-process twice, fresh scenes: bit-identical every step for free fall, stack, pile (existing asserted tests, pass) and ragdoll (new, pass).
- Cross-process: three separate invocations printed identical final hashes: free fall `584023f76efd653e`, box stack `80f096d1d4319597`, pile `7f0485268845825d` (trajectory hashes also identical); ragdoll `0b5ee0c4c0c813c6` in two separate processes.
- 0 vs 4 worker threads (flag on): bit-identical for all three scenes. Flag off, run vs run (4 threads): bit-identical. Flag off vs on: identical for free fall, **different** for box stack and pile (the flag changes results, it is not a no-op).
So: **same binary, same machine, same call sequence is repeatable**, including with 4 worker threads, which matches but does not prove the vendor's claim. [Vendor promise is only "same actors created in the same order, same time-stepping" `PxSceneDesc.h:244-245`.]

### 3.3 1 ulp perturbation, this run (Debug, local) vs CI (Release) numbers
First step with divergence above threshold; "bit-identical steps" is 0 in every case (full state hash differs from step 1).

| Scene | Perturbed | first > 1 um | first > 1 mm | first > 1 cm | max divergence (m) |
|---|---|---|---|---|---|
| free fall (1 sphere) | body 0 y | step 294 (4.9 s) | never | never | 0.000002 |
| box stack (10) | body 0 y | never | never | never | 0.000001 |
| box stack (10) | body 5 y | step 71 (1.18 s) | never | never | 0.000001 |
| pile (100) | body 0 x | step 32 (0.53 s) | step 63 (1.05 s) | step 65 (1.08 s) | 5.03 |
| pile (100) | body 50 y | step 41 (0.68 s) | step 57 (0.95 s) | step 62 (1.03 s) | 7.37 |
| **ragdoll (11 boxes, spherical joints, new)** | link 0 (pelvis) | step 52 (0.87 s) | step 59 (0.98 s) | step 73 (1.22 s) | 0.187 |
| **ragdoll (new)** | link 5 (forearm) | step 47 (0.78 s) | step 64 (1.07 s) | step 86 (1.43 s) | 0.044 |

Reading:
- Free fall and stacks: a 1 ulp nudge stays at micrometres for 10 s. **Reconciliation is enough** for these: the error is far below anything a player sees, but it is never zero, so bit determinism is still needed for hash-based desync detection.
- Pile (many contacts, tumbling): the error grows from 1 ulp to **centimetres in about 1 s** and to metres by 10 s (positions are saturated by chaos, not by float error, hence the metre scale). Any correction scheme must treat piles as unpredictable beyond ~1 s.
- Ragdoll (joints plus ground contact, one landing): micrometres at ~0.8 s, centimetres at 1.2-1.4 s, 4-19 cm by 10 s. It is chaotic like the pile, but with fewer bodies it grows slower and saturates smaller. Single ragdoll scene, one parameter set: indicative, not a statistic.
- Local vs CI: free-fall final hashes `584023f7...` (baseline) and `b4f21a3b...` (perturbed) equal the **CI Linux** values and differ from CI **Windows Release** (`9b334a56...`, divergence at step 222 not 294); box stack and pile values also differ from CI Windows. I cannot say why from this data (Debug vs Release PhysX libs, compiler contraction in the NVIDIA binary, or code changes since PR #161 head `09fc190`); it shows PhysX results change with the PhysX build configuration on one OS, which supports blocker 11. [E/M: do not rely on the cause.]
- Not measured: Linux/web in this run; PhysX Release locally; GPU. Jolt.

### 3.4 New experiment code
`tests/engine/physics/PhysXRagdollDeterminismTests.cpp` (raw PhysX API: `PxSphericalJointCreate`, 11 boxes, cone limits, PCM + stabilization + enhanced determinism + dispatcher 0 workers to mirror `PhysXBackend.cpp:649-665`):
- `PhysXRagdollDeterminismTest.TwoFreshScenesEndBitIdentical`: asserted.
- `PhysXRagdollDeterminismReportTest.OneUlpDivergence`: printed `[DETERMINISM]` lines, not asserted (same convention as PR #161).
- `PhysXRollbackSetStateReportTest.RestoreFidelityAndCost`: prints whether set-state restore replays the straight run (it does not: 0.99 m after 150 steps) and the capture/write-back time. Not asserted because the point is to document the limitation, and timings are machine dependent.
Conventions followed: gtest, `[DETERMINISM]` prefix, `#if N2ENGINE_PHYSX_ENABLED` with a skipping stub otherwise, under `tests/engine/physics/`. Nothing in CMake edited (GLOB). If the maintainers want it merged: reconfigure, build `engine_tests`, run `--gtest_filter='*PhysXRagdoll*:*PhysXRollbackSetState*'`; the CI step "PhysX determinism numbers" filter `*PhysXDeterminism*` would need `:*PhysXRagdoll*:*PhysXRollbackSetState*` added to show these lines.

## 4. Recommended project setting and editor nudge (aligned with what exists)

The model in the issue (none / authoritative+prediction / deterministic lockstep-rollback) is what `network.preset` + `network.deterministic.mode` already implement. Recommendations, as deltas over the existing design:

1. **Keep three presets, two-level for deterministic** (`mode: lockstep|rollback`), as built. The experiment supports the split: authoritative is the only viable choice with PhysX; deterministic needs Jolt.
2. **Make the choice visible at the places people decide, not only in the Network panel:**
   - Adding the first `NetworkObject`/network component with preset `none`: editor shows a non-blocking nudge "This project has no network simulation model. Pick: Server authoritative (works with PhysX) or Deterministic (needs Jolt, strict build)". Hook is `AddComponent` + a `network` tag in `ComponentSchema` (netcode design 5.3; the panel tests exist, the nudge is the part to confirm in the client repo, which I did not read).
   - Choosing **Deterministic** while `physics.backend` is `physx`, or in a non-Jolt / non-strict build: card disabled with the unmet requirement text from `ValidateNetwork` (already data-driven: the requirement `id/text/detail` triple).
   - Choosing a backend (`SetPhysicsBackend`) or `physics.gpu` / `math.simdTier` overrides that break a locked row of the active preset: show "Customised" using `LostGuarantees()` (exists).
3. **Authoritative + PhysX: tell the truth in the UI.** One info line, driven by the measurements here: "Physics is not reproducible across machines; piles diverge by centimetres within about a second, so clients are corrected from server snapshots. Predicted objects should be few and simple (characters, single bodies)." This is the authoritative preset's honest limit and costs nothing.
4. **Refuse-to-start behaviour (as built, D4):** Deterministic with any unmet requirement => `Application::Init` refuses before creating Lua, window, scene or backend; Network health row Failed with every unmet requirement; one error per requirement (docs/physics.html "network-refusal"). Add two things: (a) the **join** check must refuse a peer whose fingerprint differs in compiler/arch/strict/Jolt-cross-platform/Debug-ness (blocker 11), because the startup check is local only; (b) the desync report (netcode design 4.5) should print the same fingerprint block so a "works on my machine" Debug peer is identified at once.
5. **Authoritative mode never refuses**, only warns (GPU physics on a server, backend without set-state). Correct; PhysX declares set-state true.
6. **Do not let `physics.deterministic` imply the preset.** It stays the S1 same-binary replay option (reproducible tests, replays); the Deterministic preset requires more. The docs already say this (docs/physics.html line 246); keep the editor label "Reproducible replays (same build)" distinct from "Deterministic network model".

## 5. Recommendation

- Authoritative preset: **PhysX is adequate**; ship it as is. Set-state works and is cheap (tens to hundreds of microseconds for 60 bodies, Debug); reconciliation absorbs chaos.
- Deterministic/rollback preset: **Jolt** (already built, declared cross-platform, snapshot with contact caches, corpus with five-leg equality incl. wasm and arm64). PhysX rollback is not viable (section 2, measured). Whether a custom engine beats Jolt is #156's question; nothing found here requires one for determinism.
- Do the stage-1 items (default strict with Jolt, unordered-iteration scan, wall-clock lint, fingerprint check) before anything else; they are small and CI-checkable.

## Could not verify / not done
- Jolt: no build, so no run-to-run, 1-ulp, thread-pool or snapshot-cost numbers. Needs `-DN2ENGINE_USE_JOLT=ON` in a fresh or refreshed build; the `external/JoltPhysics` tree is present.
- The in-tree build of `PhysXRagdollDeterminismTests.cpp` (only the standalone compile/run; same source file, same PhysX libs).
- PhysX Release local numbers, Linux and web numbers in my own runs (used CI numbers from `physx-determinism-measured-numbers.md` for reference only; they are from head `09fc190`, older than master).
- Whether the join fingerprint already carries strict/cross-platform flags (`NetworkFingerprint.hpp` not read in detail).
- Editor client (Electron) side of the nudge; only the host tests were seen.
- PhysX serialization cost for a scene restore; cross-CPU-family equality for Jolt; denormal handling on every stepping thread (blocker 9).
- Vendor claims for Unity DOTS, Havok, Box2D v3, Rapier, Photon Quantum, GGPO stay as in the first report's section 2 (`[M]`); not re-verified here.
