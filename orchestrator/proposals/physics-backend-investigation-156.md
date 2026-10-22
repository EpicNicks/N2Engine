# Investigation and design proposal: a deterministic physics backend (#156, with #157)

Status: research and design only. No engine change, nothing built or run. N2Engine `master` at `ffd5a27` (Merge #189, 2026-10-20), clean checkout; netcode A2 (`SimulationClock`, PR #186) and web stage 5 (PhysX on WebAssembly, PR #184) are merged.
Inputs: `proposals/physx-limitations-and-determinism.md` (#157 report: 4.2 preset table, 4.4, 5 blocker inventory, 6), `proposals/physx-determinism-measured-numbers.md` (PR #161 CI numbers), `proposals/netcode-runtime-design.md` (sections 3-5, 4.8, track C).
Marks: **[V]** verified in this repo (path:line, relative to `N2Engine/`) or in a vendor source/doc I actually read (URL given); **[M]** from memory or general knowledge, unverified; **[E]** my estimate.

User decisions this document takes as fixed (2026-10-20): determinism target is **cross-platform** (Windows, Linux x86_64/arm64 and web/wasm peers bit-identical); **D6**: evaluate Jolt Physics (3D) and/or Box2D v3 (2D) before any custom engine; two built-in network presets: Server authoritative (PhysX) and Deterministic lockstep/rollback, which requires a backend declaring `DeterminismLevel::CrossPlatform` and `SupportsSnapshot`.

Vendor sources read for this report (2026-10-20):
- Jolt `Docs/Architecture.md` (master), sections "Deterministic Simulation", "Rolling Back a Simulation", "Being Sloppy...", "Character Controllers", "Big Worlds": https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md (rendered: https://jrouwe.github.io/JoltPhysics/)
- Jolt `Build/CMakeLists.txt`, `Jolt/Jolt.cmake`, `Jolt/Physics/StateRecorder.h`, `Jolt/Physics/PhysicsSystem.h`, `Jolt/Physics/Body/BodyCreationSettings.h`, `Jolt/Physics/Collision/ContactListener.h`, `Jolt/Math/Trigonometry.h`, `Jolt/Core/JobSystemSingleThreaded.h` (raw.githubusercontent.com, master); README https://github.com/jrouwe/JoltPhysics; latest release **v5.6.0, 2026-07-11** (GitHub API).
- Box2D README, `include/box2d/box2d.h`, top-level `CMakeLists.txt` (main): https://github.com/erincatto/box2d; release notes **v3.2.0, 2026-10-19** (GitHub API); blog "Determinism" https://box2d.org/posts/2024/08/determinism/
- Rapier determinism guide https://rapier.rs/docs/user_guides/rust/determinism/

---

## Executive summary

- **Recommendation: integrate Jolt Physics, built with `CROSS_PLATFORM_DETERMINISTIC`, as a second 3D backend behind `IPhysicsBackend`; keep PhysX as the default and the backend of the Server-authoritative preset; do not write a custom engine now.** Jolt's own documentation promises results "deterministic regardless of" compiler (MSVC 2022, clang, gcc, emscripten), configuration, OS, architecture and word size with that option, about 8 % slower, and lists its per-build verification set, which covers every leg we need: Windows MSVC x86-64, Linux clang/gcc x86-64 and ARM64, and "WASM32 emscripten running in nodejs" [V Jolt Architecture.md "Deterministic Simulation"]. It has `PhysicsSystem::SaveState/RestoreState` through a `StateRecorder` that saves bodies **and contacts** (warm-start cache) and constraints, documented for rollback [V `StateRecorder.h:20-28`, `PhysicsSystem.h:165,168`, Architecture.md "Rolling Back a Simulation"]. MIT licence [V README].
- **The engine's physics surface is small, which makes a second backend cheap:** sphere, box and capsule colliders (several per body, with offsets), static/dynamic/kinematic bodies, mass with derived inertia, 32 layers with a collision matrix, triggers, Enter/Stay/Exit events with contact points, raycast/raycast-all/sphere-cast [V `IPhysicsBackend.hpp:61-205`, `Layers.hpp:22`]. No joints, meshes, height fields, CCD or character controller are used anywhere [V: no `Joint|MeshCollider|eENABLE_CCD|PxController|ConvexMesh|HeightField` in `engine/`; `docs/web-build.html` "What the engine uses" table]. All gameplay code reaches physics only through `Application::Get3DPhysicsBackend()` [V grep: `Rigidbody.cpp`, `ICollider.cpp`, `Raycast.cpp`, `LuaPhysicsBindings.cpp:198`, `Layers.cpp:271`]; no PhysX type leaks outside `engine/src/physics/physx/` except the one `make_unique<PhysXBackend>` in `Application.cpp:216` [V].
- **Box2D v3 is the right 2D engine, but 2D is a separate, later track.** The engine has no 2D physics at all (no `Rigidbody2D`/`Physics2D`/Box2D anywhere [V grep]); a 2D backend needs its own component set and seam, not just a backend. Box2D claims cross-platform determinism by default (FMA off, own trig) [V blog, `CMakeLists.txt:49-60`] and v3.2.0 (released 2026-10-19, one day ago) added world snapshots, explicitly **experimental** with a format that "will change" [V `box2d.h:324-367`].
- **Strict float, not fixed point.** Both candidate engines are strict-float; Lua numbers are doubles [V `external/lua/luaconf.h:119,156`]; the renderer, math library and serialization are float. Fixed point would force a custom engine (months) and a parallel math stack; strict float costs the engine-side items already planned (strict-FP build exists [V `CMakeLists.txt:30-66`], scalar tier setting exists [V `Application.cpp:141-153`], engine-owned trig still missing [V `Quaternion.cpp:21-36`, `Matrix.hpp:309-334`]) plus a CI proof.
- **Proof is CI, not argument:** a physics scene corpus replayed on Windows MSVC, Linux clang x86-64, Linux clang arm64, Linux GCC 14 and web/node, hashes aggregated and compared with committed goldens (extends netcode C2). Today no Windows leg builds with `N2ENGINE_STRICT_FP` (only the GCC leg does [V `.github/workflows/tests.yml:636`]); that has to change.
- **Cost [E]:** Jolt track ~10 PRs (one L-sized backend split over 3-4 PRs, by comparison PhysXBackend is 2,418 + 566 lines [V `wc`]); a custom 3D engine of comparable quality ~25-40 PRs plus open-ended stacking/robustness tuning. Exit criteria that would revive the custom option are in 6.4.

---

## 1. Requirements for a deterministic backend

### 1.1 Derived requirements and what `IPhysicsBackend` needs
Sources: #157 4.2/4.4/5, netcode 3.3, 3.5, 4.2-4.5, 4.8. "Today" = the current interface/PhysX backend.

| # | Requirement | Why (preset/netcode) | Today in the engine | What the seam needs |
|---|---|---|---|---|
| R1 | **Cross-platform bit-exactness** (Win MSVC, Linux clang/gcc x64 + arm64, wasm) | Deterministic preset, D2 | PhysX differs Windows vs Linux for every scene, even free fall [V measured-numbers:44-46]; web differs from both [V `docs/web-build.html` "Determinism against native"] | Backend declares `DeterminismLevel` (`None/SameBinary/CrossPlatform`); not in the code yet [V: no `DeterminismLevel` in `engine/include`] (netcode A1) |
| R2 | **SaveState/RestoreState** (full world, incl. warm-start/contact caches) | Rollback (netcode 4.2, 4.4) | Not possible with the PhysX scene API (#157 row 2) | `SupportsSnapshot()`, `SaveState(bytes&)`, `RestoreState(span)` |
| R3 | **Set-state per body** (teleport dynamic, sleep state) | Authoritative reconciliation (netcode 3.5) | Dynamic bodies cannot be teleported: `Rigidbody` ignores transform changes of dynamic bodies [V `Rigidbody.cpp:334-339`], `SetBodyTransform` sets a kinematic target [V netcode 3.5] | `BodyState` get/set + `SupportsSetState()` (netcode B5); every backend |
| R4 | **Fixed step, tick-driven** | Both presets | `SimulationClock`, `Time::GetTick`, timeScale-independent dt in network modes merged (PR #186) [V `SimulationClock.hpp:12-106`, `Application.cpp:669-674`]; physics stepped with `Time::GetFixedDeltaTime()` [V `Application.cpp:616`] | Nothing new; backend must not sub-step on wall clock |
| R5 | **Thread-count independence or single thread** | B7 | PhysX: bit-identical 0 vs 4 threads on one platform [V measured-numbers:37-42]; web has no threads [V web-build.html:150-155] | Backend reports it; Deterministic preset may force a single-threaded job system |
| R6 | **Ordered iteration everywhere** (creation, events, sync, queries) | B3 | PhysX backend already orders touch maps by handle (`TouchMap` is a `std::map` keyed by handle [V `PhysXBackend.hpp:411-473`]); `_colliderShapes` is pointer-keyed `unordered_map` [V `:336`] | New backend: handle-indexed vectors, sorted event dispatch, sorted query results |
| R7 | **Own transcendental functions, no FMA, no fast-math, consistent FTZ/DAZ** | B2, B8 | `N2ENGINE_STRICT_FP` exists, default OFF [V `CMakeLists.txt:34-66`]; engine math still uses `std::sin/cos/acos/atan2` [V `Quaternion.cpp:21-197`, `Matrix.hpp:309-872`, `Vector2/3/4.hpp`]; no code sets MXCSR/FPCR [V grep `_mm_setcsr|_MM_SET_FLUSH_ZERO|_controlfp`: none outside `external/`] | Backend's own math deterministic; engine trig for gameplay (netcode C1) |
| R8 | **Deterministic broadphase and solver ordering** | R1 | n/a | Inside the engine library (Jolt/Box2D provide; custom must design) |
| R9 | **Scene queries**: raycast, raycast-all, sphere cast (exist); **capsule cast and overlap** (new, for the controller) | Gameplay, netcode N9 | `Raycast`, `RaycastAll`, `SphereCast` only [V `IPhysicsBackend.hpp:183-205`] | Add `CapsuleCast`, `OverlapSphere/Box/Capsule` to both backends; results sorted (distance, then handle) |
| R10 | **Trigger and contact events** with the engine's semantics (per body pair, Enter when the first shape pair touches, Exit when the last stops; contact points, impulses) | Gameplay; docs `physics.html` "Per-shape tracking" | `Collision` has contact points with `normalImpulse`, `tangentImpulse`, `relativeVelocity`, `impulse` [V `PhysicsTypes.hpp:22-79`] | Same event semantics from the new backend; dispatch order by handle |
| R11 | **Materials**: static/dynamic friction, restitution per collider | `PhysicsMaterial` [V `PhysicsMaterial.hpp:10-41`] | PhysX materials cached per value [V `PhysXBackend.hpp:364-389`] | Mapping documented (Jolt and Box2D have one friction coefficient, see 4.3) |
| R12 | **Layers / collision matrix** (32 layers, runtime changes re-filter pairs) | `Layers.hpp:22` [V], `SetColliderLayer`/`RefreshCollisionMatrix` [V `IPhysicsBackend.hpp:153-159`] | PhysX filter data + `resetFiltering` | Jolt: object layer per engine layer + pair filter from the matrix; re-filter semantics re-implemented |
| R13 | **Joints, CCD** | Not used by the engine today [V] | none | Not required for v1; nice to have (Jolt and Box2D both have them [V READMEs]) |
| R14 | **2D and 3D** | 2D games (D6 includes Box2D) | No 2D at all [V] | 2D needs its own components/seam (section 4.8) |
| R15 | **WASM size and single-thread speed** | Web peers | PhysX added +2.74 MB raw / +0.77 MB brotli to the size profile [V `web/budgets.json:2`, web-build.html size table]; 0.81 ms/step for 200 boxes under node, dev profile [V web-build.html "Threads, memory"] | Measured per backend in CI (budget check exists [V `scripts/web-budget-check.mjs` referenced in budgets.json]) |
| R16 | **Licence compatible, notices** | #151 | PhysX BSD-3 [V web-build.html "Licence"] | MIT (Jolt, Box2D) [V READMEs]; THIRD_PARTY_NOTICES entry |
| R17 | **Maintenance**: active upstream, pinnable, buildable from source on all legs | CI policy | PhysX: Windows prebuilt, Linux/web built by us [V `CMakeLists.txt:89-130`] | Source vendored, one build path for all legs |

### 1.2 What the Deterministic preset additionally needs outside physics (not this issue, listed so cost is honest)
B4 Lua (`Ordered.pairs`, libm shims), B6 seeded RNG (done: `Random.Seed` [V netcode 4.3]), B8 engine trig (not done), B9/B11-B14 (netcode 4.3). A deterministic physics backend is necessary, not sufficient.

---

## 2. Candidates

### 2.1 Jolt Physics (3D) [recommended]
**What `CROSS_PLATFORM_DETERMINISTIC` guarantees** [V Architecture.md "Deterministic Simulation"]:
- Without it: deterministic when the modifying APIs are called in exactly the same order and "the same binary code is used".
- With it (CMake option, default OFF [V `Build/CMakeLists.txt:21`]): "approximately 8% slower but the simulation will be deterministic regardless of: Compiler used ... (tested MSVC2022, clang, gcc and emscripten); Configuration (Debug, Release or Distribution); OS (tested Windows, macOS, Linux); Architecture (x86, ARM, RISC-V, PowerPC, LoongArch); Word size (32 / 64 bit)".
- Verified every Jolt build on (subset we care about): Windows MSVC x86 64-bit AVX2, Linux clang x86 64-bit AVX2, Linux clang ARM 64-bit NEON, Linux gcc x86 64-bit AVX2, Linux gcc ARM 64-bit NEON, **WASM32 emscripten running in nodejs**, WASM64 [V same section]. Jolt itself says the feature "is less tested than other features" [V].

**Restrictions** [V same section unless marked]:
- Same Jolt source and same defines on every platform (e.g. not `JPH_DOUBLE_PRECISION` on one only).
- Application compiled precise (`/fp:precise`, clang `-ffp-model=precise`), contraction off (`-ffp-contract=off`); the option itself adds `/fp:precise` on MSVC and `-ffp-contract=off` on clang/gcc to Jolt's build [V `Build/CMakeLists.txt:204-205,261-262`] and drops `JPH_USE_FMADD` [V `Jolt.cmake:959-960,1019`]. Jolt **keeps its SIMD** (SSE/AVX/NEON) under the option: the guarantee is across architectures, so SIMD tiers are part of what Jolt claims is equal [V by the architecture list; M on how each op is made equal].
- FPU state consistent: rounding to nearest, DAZ/FTZ set consistently on all platforms and threads.
- No `std::sin/cos`, `std::sort`, `std::push_heap/pop_heap`, `std::hash` in deterministic code paths: use Jolt's `Sin/Cos/ATan2...` [V `Math/Trigonometry.h:12-74`], `QuickSort`, `BinaryHeapPush/Pop`, `Hash`.
- **Broadphase queries are NOT deterministic** (bounding boxes widened lazily, modified from several threads); **narrowphase query results are consistent but their order can change**; **ContactListener, BodyActivationListener, PhysicsStepListener callbacks come from several threads in nondeterministic order**; `GetActiveBodies` order is nondeterministic [V same section; `ContactListener.h:69-73`]. Consequence: our backend uses `NarrowPhaseQuery` only, sorts every result list, buffers and sorts callbacks, and iterates its own handle-ordered body list.
- Thread count: not stated explicitly. The verified set mixes multi-threaded native runs and single-threaded node runs, which suggests independence [M]; CI must test 0 vs N threads as #161 did for PhysX. `JobSystemSingleThreaded` exists [V `Core/JobSystemSingleThreaded.h`].

**Snapshot** [V `StateRecorder.h:20-28,88-94,121`, `PhysicsSystem.h:165,168`, Architecture.md]: `SaveState(StateRecorder&, EStateRecorderState = All, const StateRecorderFilter* = nullptr)`, `RestoreState(StateRecorder&, filter)`; flags `Global` (dt, gravity), `Bodies`, `Contacts`, `Constraints`; a filter can skip static/inactive bodies to shrink snapshots; `StateRecorder::SetValidating` compares instead of overwriting (desync debugging). Caveats: only state the engine modifies during a step is saved (a `SetFriction` inside the window must be replayed by us); adding/removing bodies inside the window breaks `RestoreState` (remove and re-add, keep `BodyID`s, or `CreateBodyWithID`). This matches the netcode "parked spawns" contract (netcode 4.4 point 3) exactly.

**Other facts**: MIT [V README]; C++17 [V `Jolt.cmake:2`]; platforms Windows x86/x64/ARM64, Linux x86/x64/ARM32/ARM64/..., WebAssembly via JoltPhysics.js [V README]; `USE_WASM_SIMD` default OFF "since not all browsers support this" [V `Build/CMakeLists.txt:64-66`], which matches our scalar web baseline (web-build.html "Design decision: scalar PhysX"); 64-bit wasm opt-in [V `:68-70`]; `OBJECT_LAYER_BITS` 16 by default [V `:51`]; C++ exceptions and RTTI OFF in Jolt's own build [V `:44,48`]; floating-point exceptions ON by default for MSVC Debug/Release [V `Jolt.cmake:840-841`] (we would turn that off: engine code can legitimately produce NaN/inf in tests); interprocedural optimisation ON by default [V `:35`]. Shapes include sphere, box, capsule, convex hull, mesh, height field; constraints (fixed, point, hinge, slider, cone, 6DOF...); CCD (`EMotionQuality::LinearCast` [V `BodyCreationSettings.h:104`]); soft bodies; `Character` and `CharacterVirtual` controllers [V README, Architecture.md "Character Controllers"]. Used by Horizon Forbidden West and Death Stranding 2 [V README]; Godot 4.4+ ships a Jolt module [M]. Who ships it **deterministically** (lockstep/rollback) could not be verified. One principal maintainer (Jorrit Rouwé) [M]: bus-factor risk, mitigated by MIT + vendored source.
**Size** [E]: Jolt's full library is large (the `Jolt.cmake` source list is 1,042 lines [V]); we would link sphere/box/capsule + compound shapes, the narrow phase and the solver; the linker drops the rest (no RTTI). Expect the same order as PhysX's +2.7 MB raw / +0.77 MB brotli, likely less; J1 measures it.

### 2.2 Box2D v3 (2D)
- Claims: "3 levels of determinism: algorithmic, multithreaded, and cross-platform", cross-platform is "the default"; tested MSVC/Clang/GCC on x64 and ARM; a CI test ("Falling Hinges") checks a transform hash on every PR; "Determinism is a brittle feature. It is easy to break." [V blog 2024-08]. WebAssembly determinism is not claimed in what I read [V by absence]; the README points to a third-party wasm port (Birch-san/box2d3-wasm) [V README].
- Restrictions: no fast-math; FMA off (`-ffp-contract=off`, and `/clang:-ffp-contract=off` for clang-cl) set by Box2D's own CMake [V `CMakeLists.txt:49-60`]; own `sin/cos/atan2` approximations (`b2MakeRot`), `sqrtf` trusted [V blog]. SIMD (SSE2/AVX2/NEON, runtime AVX2 dispatch in 3.2) stays on; `BOX2D_DISABLE_SIMD` exists [V README, release notes].
- Snapshot: v3.0 had none ("no mechanism for rolling back this internal state" [V blog]); **v3.2.0 adds `b2World_GetSnapshot`, `b2World_RestoreFromSnapshot`, `b2CreateWorldFromSnapshot`, ids stay valid on in-place restore, user data nulled, "Experimental. The snapshot API and image format will change in future versions."** [V `box2d.h:324-367`], plus recording/replay [V `box2d.h:290-320`].
- C17, MIT [V README]; joints, CCD, sensors, ray/shape casts, overlap queries, character movers [V README]; per-shape friction and restitution [M]. v3.2 changed joint and chain APIs [V release notes]: young API.
- Verdict: technically the best 2D option, but (a) 2D needs a whole component set first, (b) its snapshot is one day old and experimental. Defer to a 2D track (P6).

### 2.3 Others (why not)
- **Rapier** (Rust): `enhanced-determinism` feature gives cross-platform IEEE-754 determinism, incompatible only with `simd8`, parallel results equal sequential and independent of thread count, wasm listed [V rapier.rs determinism guide]; snapshot via serde [M]. Why not: Rust toolchain + C FFI in every leg (MSVC, clang x2, gcc, emscripten), no maintained C++ API [M], two languages in the build for one subsystem.
- **Bullet**: no cross-platform determinism mode, stateful caches, largely unmaintained for games [M].
- **ReactPhysics3D / qu3e / bounce**: small, no determinism claims, no snapshot [M].
- **Havok**: commercial licence, closed source [M]. **Unity Physics/DOTS**: C#, not embeddable [M].
- **PhysX immediate mode** (user-owned bodies and caches, `PxImmediateMode.h:111-269` [V #157 row 2]): fixes snapshots but **not** cross-platform determinism (Windows prebuilt binary vs our Linux build, measured different [V measured-numbers:44-46]) unless we also build PhysX from source everywhere with strict flags and prove PhysX's SIMD paths equal across x86/ARM/wasm, which NVIDIA does not claim. No.

### 2.4 A custom engine (what it costs)
Minimal deterministic 3D rigid-body engine matching today's engine surface (R1-R12, no joints) [E for all numbers]:

| Part | Content | Size [E] |
|---|---|---|
| Math | Deterministic scalar vec/quat/mat ops, own sin/cos/atan2/acos, sqrt (IEEE exact) | 1-1.5k lines |
| Broadphase | Sorted sweep-and-prune or dynamic AABB tree with deterministic insertion/rebalancing; pair cache sorted by handle | 1-1.5k |
| Narrowphase | sphere-sphere, sphere-capsule, capsule-capsule, sphere-box, capsule-box, **box-box (SAT + clipping, the hard one)**, compound bodies; persistent manifolds with feature ids for warm starting | 2.5-4k |
| Solver | Sequential impulse / soft-step, friction, restitution, warm start, position correction; stacking quality is weeks of tuning | 1.5-2.5k |
| Islands, sleeping | Union-find islands in handle order, sleep thresholds | 0.5-1k |
| Queries | ray vs shapes, sphere/capsule sweeps (GJK/conservative advancement or analytic), overlaps, sorted results | 1-2k |
| Events, layers, triggers, snapshot | Plain-data state: snapshot = memcpy of arrays (the one real advantage) | 1-1.5k |
| Tests + tuning | Conformance, stacking/jitter goldens, fuzz | as much again |

Totals [E]: 9-14k lines of engine code, **25-40 PRs**, 4-8 months of focused work for one developer, and the long tail (tunnelling, jitter in stacks, box-box edge cases, sleep popping) that Jolt and Box2D have already paid. A 2D-only custom engine is roughly half. **Fixed-point variant**: add a Q-format math stack (64x64->128 multiply, fixed sqrt/trig), overflow and precision tuning per game scale, conversion at every boundary: +40-60 % [E]. Advantage over Jolt: total control and a trivially copyable state. Risk: quality and schedule; nothing in the engine's requirements needs what only a custom engine can do (section 6.4).

### 2.5 What PhysX already gives the Server-authoritative preset (so it stays)
Set-state calls exist (`setGlobalPose`, `setKinematicTarget`, `setWakeCounter`, `putToSleep`, velocities [V netcode 3.5, PhysX headers]); same-binary determinism measured bit-identical run-to-run and 0 vs 4 threads [V measured-numbers:33-46], enough for server-side replays; a 1-ulp nudge in a 100-body pile passes 1 mm only after ~50 steps [V measured-numbers:28-31], far beyond a 6-12-tick prediction window; GPU particles (PBD) only exist on PhysX [V `PhysXBackend.hpp:246-249`]; PVD on Windows [V `:29-40`]; CCT, joints, meshes available if the engine ever exposes them [V CMakeLists links CharacterKinematic per netcode 3.3]. Nothing in the authoritative design needs Jolt.

---

## 3. Fixed point vs strict float (engine-wide)

| Aspect | Strict float (recommended) | Fixed point |
|---|---|---|
| Math types | Keep `Math::Vector3`/`Quaternion` (float, `alignas(16)` [V `CMakeLists.txt:18-21` comment]). Deterministic preset forces `Scalar` tier [V setting `Application.cpp:141-153`]; later bit-exact SIMD tiers | New `Fixed`, `FixedVector3`, `FixedQuaternion`, matrices beside the float ones; every sim path duplicated |
| Lua | Numbers are doubles (`LUA_FLOAT_DEFAULT = LUA_FLOAT_DOUBLE` [V `luaconf.h:119,156`]); IEEE double `+ - * / sqrt` are exact across platforms; replace `math.sin/cos/...` with engine trig (netcode 4.7); double->float conversion into physics is correctly rounded | Fixed values as userdata (slow, unergonomic) or raw integers |
| Rendering | Unchanged (floats) | Convert sim->render every frame (cheap, but every reader must remember) |
| Physics | Jolt (CPD) / Box2D are strict-float | No maintained fixed-point Jolt/Box2D [M]: forces the custom engine |
| Speed | Native float; Jolt CPD ~8 % slower [V] | 64-bit integer mul/shift; trig by tables/polynomials; typically slower for 3D [M] |
| Range/precision | Float: Jolt recommends simulation within ~5 km of origin [V Architecture.md "Big Worlds"]; same limit as the renderer | Chosen per game; overflow becomes a bug class |
| Proof burden | Empirical: requires no contraction, no fast-math, same FTZ/DAZ, own transcendental functions, same NaN handling; proven by golden hashes on every leg, re-proven on each toolchain bump | Integer ops are exact by construction; remaining risk is our own code (ordering) |
| libm | Not used in sim: Jolt uses its own `Sin/Cos/ATan2` [V `Trigonometry.h`], Box2D its own approximations [V blog]; the engine adds its own for gameplay (C1). `sqrt` is IEEE correctly rounded everywhere [M, standard] | Own functions anyway |
| wasm | IEEE single/double exact except NaN bit patterns; no FMA outside relaxed SIMD [V web-build.html "Determinism against native"] | Fine (i64 native) |
| Editor/serialization | Unchanged | New `FieldKind`s, inspector conversions |

**Recommendation: strict float engine-wide.** The two engines we would integrate are strict-float and claim (Jolt: verifies) cross-platform equality, the engine's whole math/render/script stack is float/double, and the residual risks (denormal flags, NaN payloads, compiler constant folding) are exactly what the cross-leg hash job catches. Fixed point is only the fallback inside a custom engine (6.4).

Concrete engine rules for the strict-float profile (in addition to `N2ENGINE_STRICT_FP` [V `CMakeLists.txt:34-66`]):
1. Engine-owned `Math::Sin/Cos/Tan/Asin/Acos/Atan/Atan2` (port Jolt's polynomial implementations, MIT, or write our own; keep the `math` library free of a Jolt dependency) used by `Quaternion.cpp`, `Matrix.hpp`, `Vector2/3/4.hpp` slerps, `Random.cpp` and the Lua shims, in every build (one code path, so goldens are the same in all presets) [E: S-M PR, = netcode C1].
2. A per-tick FP-environment guard in deterministic mode: set and assert round-to-nearest, FTZ/DAZ **off** (x86 MXCSR, ARM FPCR; wasm has none) before stepping, because a third-party library (audio, drivers) can change them [M]; no engine code sets them today [V grep].
3. NaN canonicalisation in state hashes (netcode B13).
4. No `std::sort` of keys with ties, `std::hash`, `unordered_*` iteration in sim paths (Jolt rule, already engine policy B3).

---

## 4. Integration design (Jolt behind `IPhysicsBackend`)

### 4.1 Build
- `external/JoltPhysics/` vendored at **v5.6.0** (only `Jolt/`, `LICENSE`, `Build/` CMake pieces we need), like `external/lua` and `external/mbedtls` [V `external/` listing]. Included via `Jolt/Jolt.cmake` with our own options: `CROSS_PLATFORM_DETERMINISTIC=ON`, `DOUBLE_PRECISION=OFF`, `FLOATING_POINT_EXCEPTIONS_ENABLED=OFF`, `ENABLE_OBJECT_STREAM=OFF`, `INTERPROCEDURAL_OPTIMIZATION=OFF` (one LTO policy for the whole engine), static MSVC runtime matching `CMAKE_MSVC_RUNTIME_LIBRARY` [V `CMakeLists.txt:12-14`], `USE_AVX2/USE_AVX512 OFF` on x86 to keep the engine's AVX-only CPU floor [V `math/CMakeLists.txt:34-45`] (Jolt's claim makes the SIMD level irrelevant to results), `USE_WASM_SIMD=OFF` on web.
- CMake option `N2ENGINE_USE_JOLT` (default ON once J3 lands; OFF builds keep compiling, like `N2ENGINE_USE_PHYSX` [V `CMakeLists.txt:25`]). Both backends can be linked into one binary; only one is instantiated.
- Third-party notices entry (#151 precedent).

### 4.2 Selection, coexistence, declaration
- `ApplicationOptions::PhysicsBackend` gains `JOLT` and `AUTO` [V today only `PHYSX`, `ApplicationOptions.hpp:8-11`; four hosts hard-code `PHYSX`: `editor-server/src/Host.cpp:340`, `lua_project/src/Main.cpp:102`, `web/player/WebPlayer.cpp:374`, `test_project/src/Main.cpp:301` V]. New project setting `physics.backend: "physx" | "jolt"` read in `Application::Init` next to `physics.gpu`/`physics.deterministic` [V pattern `Application.cpp:202-219`, parse at `ProjectSettings.cpp:25-50`]; hosts pass `AUTO` = "project decides, PhysX if unset".
- **One backend per project (process), not per scene**: the backend is created once in `Init` and lives across scene switches [V `Application.cpp:213-245`, `:515`]; a per-scene backend would need teardown/rebuild on every scene load and a per-scene editor setting for no use case. The network preset table locks it: Deterministic => `jolt` (L, S), Authoritative/None => any.
- Declarations (netcode A1 adds the API; this design fills it): `PhysXBackend`: `None` (or `SameBinary` with `physics.deterministic`), `SupportsSetState` true after B5, `SupportsSnapshot` false. `JoltBackend`: `CrossPlatform` **only if** compiled with `JPH_CROSS_PLATFORM_DETERMINISTIC` and `N2_STRICT_FP`, else `SameBinary`; `SupportsSnapshot` true; `SupportsSetState` true. Also `BackendId()` ("physx-5.6.1", "jolt-5.6.0+cpd") for the determinism fingerprint (netcode 2.2).
- GPU: `JoltBackend::SetGpuMode` ignored (default no-op [V `IPhysicsBackend.hpp:28`]); health row says "Jolt (CPU)".

### 4.3 Component mapping
| Engine | Jolt |
|---|---|
| `Rigidbody` Static/Dynamic/Kinematic [V `Rigidbody.hpp:12-17`] | `EMotionType::Static/Dynamic/Kinematic`; kinematic moved with `MoveKinematic` (velocity-based, pushes dynamics); static moved with `SetPositionAndRotation` |
| Several colliders per body, each with `localOffset` [V `IPhysicsBackend.hpp:104-148`] | One `MutableCompoundShape` per body (or `StaticCompoundShape` rebuilt on change); sub-shape user data = collider index; single collider at origin = plain shape |
| Sphere/Box/Capsule (`radius`, `halfExtents`, `radius+height`) | `SphereShape`, `BoxShape` (convex radius chosen <= min half-extent), `CapsuleShape(halfHeightOfCylinder, radius)`; axis convention converted (Jolt capsule along Y [M]) |
| Mass authoritative, inertia from non-trigger shapes rescaled [V `Rigidbody.hpp:47-50`] | `EOverrideMassProperties::CalculateInertia` + mass override [V enum field `BodyCreationSettings.h:118`; M on exact enum value names]; sensors excluded from mass |
| `SetGravityEnabled` | `mGravityFactor` 0/1 [V `BodyCreationSettings.h:113`] |
| `SetBodyEnabled` | `BodyInterface::RemoveBody/AddBody` (ID kept), pairs end with Exit |
| Trigger collider | Jolt sensors are per body (`mIsSensor` [V `:100`]): a body mixing trigger and solid colliders becomes **two Jolt bodies** (solid + sensor body moved with it), or per-sub-shape filtering in `OnContactValidate` [M on feasibility]; spike in J4 |
| `PhysicsMaterial` static/dynamic friction, restitution [V] | Jolt has **one friction and one restitution per body** [V `BodyCreationSettings.h:107-108`]; per-collider values applied in `OnContactAdded/Persisted` by writing `ContactSettings::mCombinedFriction` [V `ContactListener.h:43`] from the touching sub-shapes. Static friction: no equivalent; map `dynamicFriction` and document. Combine rule: average, as PhysX's default [M] |
| Layers 0-31 + matrix [V `Layers.hpp:22-33`] | `ObjectLayer` = engine layer (+ static/moving bit), `ObjectLayerPairFilter` reads the matrix, 2 broad-phase layers (static, moving); matrix change = filter swap + re-validate touching pairs, reproducing the PhysX backend's re-filter Exit/Enter semantics |
| Events (Enter/Stay/Exit per body pair, contact points, impulses) | `ContactListener` (multi-threaded [V `ContactListener.h:69-73`]) pushes into a mutex-protected buffer; after `Update`, sort by (handle pair, sub-shape pair) and fold into the same `TouchMap` logic the PhysX backend uses (factor it into a backend-neutral `ContactTracker`). **Gap:** callbacks run before the solve, so applied impulses are not available there [M]; `ContactPoint::normalImpulse` would be 0 or estimated: open question Q3 |
| Queries | `NarrowPhaseQuery::CastRay` (closest: custom collector, ties by handle), `CastRay` all-hits sorted by (distance, handle), `CastShape` with sphere/capsule for sweeps, `CollideShape` for overlaps; never `BroadPhaseQuery` (not deterministic [V]); `QueryTriggers::Ignore` via body filter on sensors |
| `SyncTransforms` | Iterate our handle-ordered `_bodies`, not `GetActiveBodies` (nondeterministic order [V]) |
| Body ids | `BodyInterface::CreateBodyWithID(BodyID(handle.index))` so ids follow our deterministic free list [V `PhysXBackend.hpp:334-335,411-414`] and match across peers |
| Threads | Native: `JobSystemThreadPool` (N workers) unless the Deterministic preset forces `JobSystemSingleThreaded` (until CI proves thread-count independence); web: single-threaded [V `N2ENGINE_SINGLE_THREADED`, web-build.html:150] |

Reuse: `Rigidbody`, colliders, `Raycast`, Lua bindings and editor schemas do not change; only the factory in `Application::Init` and the new backend. The PhysX conformance tests already install a backend through `Set3DPhysicsBackend` [V `tests/engine/physics/PhysicsTests.cpp:114-116`, `LayerPhysicsTests.cpp:61-63`, `PhysicsOrderTests.cpp:53-55`], so they can be turned into typed tests over both backends (1,026 + 483 + 161 lines [V `wc`]).

### 4.4 Snapshot/restore API (seam additions)
```cpp
enum class DeterminismLevel : uint8_t { None, SameBinary, CrossPlatform };   // netcode A1
virtual DeterminismLevel GetDeterminism() const { return DeterminismLevel::None; }
virtual bool SupportsSetState() const { return false; }
virtual bool SupportsSnapshot() const { return false; }
virtual std::string_view BackendId() const = 0;
struct BodyState { Vector3 position; Quaternion rotation; Vector3 linearVelocity, angularVelocity; bool sleeping; };
virtual bool GetBodyState(PhysicsBodyHandle, BodyState&) const;               // netcode B5
virtual bool SetBodyState(PhysicsBodyHandle, const BodyState&);              // teleport dynamic, target kinematic
virtual bool SaveState(std::vector<std::byte>& out) const;                   // between steps only
virtual bool RestoreState(std::span<const std::byte> in);                    // same body set as at save
virtual std::uint64_t HashState() const;                                     // canonical, handle order
virtual bool CapsuleCast(...); virtual int Overlap(...);                     // R9, both backends
```
- `JoltBackend::SaveState` = Jolt `SaveState(All)` bytes + backend bookkeeping that affects future events and results: the `ContactTracker` touch maps, pending Exit queues, per-body enabled flags, gravity, layer-matrix revision. `RestoreState` refuses (returns false, logs) if the live body set differs from the snapshot's (spawns/despawns inside the window must be parked by netcode, 4.4 point 3, via `SetBodyEnabled(false)` which keeps the `BodyID` [V Jolt rollback text]).
- `HashState` hashes body states in handle order with NaN canonicalised (independent of Jolt's byte layout, so it also works for PhysX same-binary replays).
- Validation aid: Jolt's `StateRecorder::SetValidating` [V] gives a "where did it diverge" diff for desync reports (netcode 4.5).

### 4.5 How netcode rollback uses it
Per confirmed tick T: `SaveState` into a ring of N (= rollback window, default 8 [V netcode 4.2]) preallocated buffers (`StateRecorderFilter` skips static bodies [V `StateRecorder.h:88`]). On a late input for tick T-k: `RestoreState(ring[T-k])`, restore non-physics sim state (netcode 4.4), re-run k ticks with corrected inputs (`StepTicks` exists from A2 [V commit 321a0bb]), events of the replayed ticks are rollback-aware (netcode 4.4 point 4). Cost [E]: a few µs per body per save/restore; must be measured (6.4 criterion b).

### 4.6 Soft bodies and particles without PhysX
Unchanged: `GetParticleBackend()`/`GetSoftBodyBackend()` default to `nullptr` [V `IPhysicsBackend.hpp:54,59`], and selection falls back to the CPU backends [V `ParticleBackendSelection.hpp:46`, `SoftBodyBackendSelection.hpp:48`], exactly as on the web and Linux today. `CpuSoftBodyBackend` collides only with an optional ground plane and never queries the physics world [V `CpuSoftBodyBackend.hpp:36-42`]; CPU particles do not query physics [V grep]. Both are cosmetic in the Deterministic preset (netcode B12). GPU PBD particles are PhysX-only: a Jolt project gets CPU particles (health row says so).

### 4.7 Character controller
Netcode N9 recommends an engine-owned kinematic controller over backend capsule casts/overlaps. That still holds with Jolt: it runs identically on both backends, and its determinism is ours to keep (sorted query results). Jolt's `CharacterVirtual` (slopes, stairs, moving platforms, built purely on narrow-phase queries [V Architecture.md "Character Controllers"]) is a tempting shortcut but Jolt-only, and its cross-platform determinism is covered by the general claim only if its internal contact ordering is stable [M]; keep it as a spike option, not the plan (P8).

### 4.8 2D (Box2D) later
A separate seam `IPhysics2DBackend` (`Vector2` + angle) and components `Rigidbody2D`, `Box/Circle/CapsuleCollider2D`, `Physics2D` queries, `Layers` shared; one 2D backend (Box2D v3.x) both for normal and deterministic use; snapshot via `b2World_GetSnapshot` once it leaves "experimental". Locked-axis 3D (Jolt `EAllowedDOFs` [M]) is an interim alternative for "2.5D" games, not a 2D story.

### 4.9 Editor
Project Settings > Physics: backend picker (PhysX / Jolt) with a one-line difference list; Network panel's Deterministic card becomes selectable when the backend declares `CrossPlatform` (D3 badge text changes to "requires the Jolt backend"); `GetEngineHealth` physics row detail names backend + determinism level. Protocol is at 1.14.0 [V `Protocol.hpp:14`]; settings travel through existing `SetProjectSettings`, the schema addition is a minor bump plus a frontend PR.

---

## 5. Proof plan (CI)

### 5.1 Scene corpus (headless, `TickOptions` stepping [V netcode 2.4], 600 ticks at 60 Hz, hash every 60 ticks)
1. Free fall (1 sphere); 2. box stack 10; 3. pile 100 mixed spheres/boxes/capsules (same LCG construction as #161, one draw per statement [V measured-numbers:11-13]); 4. compound bodies with offsets + mass override; 5. kinematic platform pushing dynamics; 6. triggers and layers, with a mid-run matrix change (event log hashed: tick, kind, handles); 7. queries: 64 rays, raycast-all, sphere and capsule casts, overlaps each tick (results hashed); 8. spawn/despawn and enable/disable mid-run; 9. **snapshot round trip**: save at 200, run to 400 (H1), restore at 200, run to 400 (H2), assert H1 == H2 and equal to the straight run; 10. rollback simulation: 8-tick rewinds every 10 ticks with identical inputs equals the straight run; 11. thread counts 0 vs 4 (native).

### 5.2 Legs and flag matrix
| Leg | Compiler | Today | Needed |
|---|---|---|---|
| windows Release | MSVC `cl` [V tests.yml:248] | PhysX ON, strict FP **off** | `N2ENGINE_STRICT_FP=ON` on this leg (or a new one) + Jolt |
| linux x86_64 | clang-20 [V :343-352] | strict off | strict ON + Jolt |
| linux arm64 | clang-20 [V :468-477] | strict off | strict ON + Jolt |
| linux x86_64 GCC 14 | gcc-14 [V :590-599] | **strict ON**, PhysX OFF [V :636] | + Jolt |
| web | emscripten, node, dev profile [V :788, :833] | PhysX ON | strict ON + Jolt; ideally also run the corpus from the `speed` profile build |
Windows Debug legs stay as they are (Jolt claims configuration independence [V], but engine glue in Debug is not part of the claim; goldens come from Release).

### 5.3 Golden hashes and aggregation
- Each leg writes `determinism-hashes.json` (scene, tick, hash, build fingerprint) as an artifact; a new `determinism` job (`needs:` all legs) downloads and compares: all legs equal **and** equal to `tests/golden/physics-determinism.json`. Mismatch output names the first scene/tick per leg. Extends netcode C2 (physics-free canaries) with physics scenes.
- Goldens change only in a PR that says why (Jolt bump, intended behaviour change); a script regenerates them from the Linux leg; reviewers see the diff.
- Non-required at first (netcode N15), required after it has been green for two weeks (P9).
- Same-leg negative control: the corpus also runs PhysX and prints its hashes (expected to differ across legs, as measured), which proves the job can detect a difference.

---

## 6. Cost/benefit and staged PR plan

### 6.1 Benefit
Unblocks the Deterministic preset (lockstep now, rollback with snapshots), gives an in-tree, source-built physics on all five legs with one build path (no 1.4 GB vendor zip on Windows [V #157 row "Size"]), snapshot-based debugging (`SetValidating`), and a path to joints/CCD/meshes later. PhysX remains for GPU particles and the authoritative preset.

### 6.2 Cost and risk summary [E]
~10 PRs, roughly 5-8k changed lines (backend ~2.5-3.5k, tests ~2k, CI/CMake/editor/docs the rest); web size +1-3 MB raw if both backends are linked; ongoing: Jolt bumps re-baseline goldens. Main technical risks: event semantics parity (R10), trigger mixing (sensors per body), contact impulses (Q3), cross-leg equality in **our** glue code.

### 6.3 Staged PR plan (S < 400 changed lines, M 400-1200, L > 1200; docs in each PR as usual)
| PR | Content | Size | CI verifies | Needs the user |
|---|---|---|---|---|
| J1 | Vendor Jolt v5.6.0 (pruned), `Jolt.cmake` include with the 4.1 options, `N2ENGINE_USE_JOLT` (default OFF), smoke test (one sphere falls) on every leg incl. web, size report, notices | M | Builds on 5 legs + web; smoke test; size numbers printed | Accept the vendored tree and the measured web size (budget raise if linked by default) |
| J2 | Seam additions: `DeterminismLevel`, `Supports*`, `BackendId`, `GetBodyState/SetBodyState` (PhysX implementation), `physics.backend` setting, `PhysicsBackend::AUTO/JOLT`, health detail (merge with netcode A1/B5 if those land first) | M | Settings parsing, PhysX set-state tests | - |
| J3 | `JoltBackend` core: bodies, compound shapes, mass, gravity, forces/velocities, enable/disable, kinematic/static moves, transform sync; `PhysicsTests` turned into typed tests over both backends | L | Conformance suite on both backends | Accept listed behaviour differences (friction model) |
| J4 | Layers/matrix, triggers (sensor strategy from the spike), contact/trigger events via a shared `ContactTracker`; `LayerPhysicsTests` + `PhysicsOrderTests` typed | L | Event semantics equal to PhysX | - |
| J5 | Queries: raycast, raycast-all, sphere cast on Jolt; `CapsuleCast` + overlaps on both backends; Lua `Physics.*` additions | M | Query tests both backends; sorted results | - |
| J6 | `SaveState/RestoreState/HashState` on Jolt, parked-body contract, snapshot round-trip and rollback-equality tests; same-binary 0 vs N thread test | M | Round trip, rewinds, thread equality | - |
| J7 | Strict-FP on the Windows Release, Linux clang x64/arm64 and web legs; corpus runner; `determinism` aggregation job with goldens (non-required) | M | Itself (cross-leg equality) | Accept CI time; later make it required |
| J8 | Engine trig + FP-environment guard (= netcode C1 items 1-2 of section 3) if not already merged | M | Exact-value tests all legs | - |
| J9 | Editor: physics backend picker, Deterministic card enabled by declaration, protocol minor bump; frontend PR | M + M | DOM tests (frontend CI), engine schema tests | Live editor check |
| J10 | Optional: web packages link only the selected backend (size), Jolt default-backend evaluation report (perf vs PhysX on the corpus) | M | Budgets, perf numbers printed | Decide on default backend |
Ordering: J1 -> J2 -> J3 -> J4/J5 (parallel) -> J6 -> J7 (J8 any time before J7 is made required) -> J9 -> J10. The Deterministic preset becomes selectable after J7 is green on all legs (netcode C4 = J3-J7).

2D track (deferred, P6) [E]: D1 vendor Box2D v3.x; D2 2D components + `IPhysics2DBackend`; D3 events/queries; D4 snapshot + corpus 2D scenes; D5 editor. 5-7 PRs.

### 6.4 Exit criteria that would trigger a custom engine instead
Any of, after J7 and with a reasonable fix attempt (including an upstream issue to Jolt):
- (a) **Cross-leg mismatch inside Jolt** on the corpus that cannot be traced to our flags/glue and is not fixed upstream within ~4 weeks [E].
- (b) **Snapshot cost**: save + restore of a 300-body scene above ~0.5 ms native or ~2 ms on web per rollback step [E thresholds], making an 8-tick window unaffordable.
- (c) **Size**: Jolt-only web size profile above ~2x PhysX's measured contribution with no pruning option.
- (d) **Event/feature parity** gaps the engine cannot work around (e.g. triggers on mixed bodies, contact impulses) that matter to the user.
- (e) **Upstream abandoned** (no release for 18 months) or licence change.
- (f) A requirement for platforms without IEEE-754 conformance, which would push to fixed point.
For 2D the analogous criteria apply to Box2D; a 2D custom engine is about half the 3D cost.

---

## 7. Risks and open questions
- Q1 **Our glue breaks determinism** (an `unordered_map` iteration, `std::sort` with ties, engine SIMD math in conversions). Mitigation: glue only copies floats, sorts with total orders, compiled under strict FP; J7 catches the rest.
- Q2 **Thread-count independence** of Jolt is implied, not stated [M]; until J6 proves it, the Deterministic preset forces `JobSystemSingleThreaded`.
- Q3 **Contact impulses**: Jolt's `ContactListener` runs before the solver [M]; `Collision::impulse`/`ContactPoint::normalImpulse` may need a post-step read or be documented as unavailable on Jolt.
- Q4 **Triggers on mixed bodies**: Jolt sensors are per body [V]; the two-body workaround changes body counts (affects snapshots and ids). Spike first in J4.
- Q5 **FTZ/DAZ set by another library** in the process (audio, GPU drivers) [M]; guard per tick.
- Q6 **Toolchain bumps** (MSVC, clang-20 -> 21, emscripten) can change results [V Box2D: "brittle"]; goldens make it visible, not impossible.
- Q7 **Jolt bus factor** [M] and pace of API change (v5 line); vendored and pinned.
- Q8 **Box2D snapshot experimental** [V]: format may change, so 2D rollback waits.
- Q9 **Behaviour drift between backends**: games tuned on PhysX feel different on Jolt (friction combine, solver). Documented; the Deterministic preset is a project-level choice made early.
- Q10 **Dual linking size** on the web if one generic player carries both backends (J10).
- Q11 Whether Jolt's CPD claim holds for the `dev` web profile (debug-ish flags) as for Release: Jolt says configuration-independent [V], to be confirmed by J7.

## Could not verify
- Jolt thread-count independence (not stated in Architecture.md), the internal mechanism that makes SIMD paths equal across x86/ARM/wasm, and whether `CharacterVirtual` results are ordered deterministically.
- Jolt's contact-impulse availability in callbacks, exact `EOverrideMassProperties` usage for "mass given, inertia computed", capsule axis convention, `CreateBodyWithID` semantics beyond the doc sentence, `EAllowedDOFs` name.
- Any game shipping Jolt with lockstep/rollback; Godot's Jolt module (memory only).
- Box2D cross-platform determinism on WebAssembly (not in the blog or README I read); whether thread-count independence is claimed (the blog shows runs "with various thread counts" without stating it); per-shape friction details.
- Rapier snapshot/serialization details (separate page not read); Bullet/ReactPhysics3D/Havok statements (memory).
- Web size of a Jolt build (no build was run); snapshot sizes and timings.
- PhysX's default friction combine mode (average) and whether `sqrt` is correctly rounded on every target (IEEE requires it; not checked per toolchain).
- Whether other libraries in the engine process change FP control flags.
- Box2D v3.2 was released 2026-10-19; I read `main`, which may already differ from the tag.

## Decisions for the user
- **P1** Integrate Jolt Physics (v5.6.0, `CROSS_PLATFORM_DETERMINISTIC`) as the deterministic 3D backend behind `IPhysicsBackend`, and do **not** start a custom engine unless an exit criterion in 6.4 is hit? Recommendation: yes.
- **P2** Build Jolt always with `CROSS_PLATFORM_DETERMINISTIC` ON (one configuration, ~8 % slower) rather than only in strict-FP builds? Recommendation: always ON.
- **P3** Vendor Jolt's source pinned in `external/JoltPhysics` (pruned to `Jolt/` + licence), not a submodule or FetchContent? Recommendation: vendor.
- **P4** Backend chosen per project (`physics.backend`, read at Init), not per scene, with the Deterministic preset locking it to Jolt? Recommendation: per project.
- **P5** PhysX stays the default backend and the Server-authoritative backend; Jolt is opt-in (forced by the Deterministic preset), with "Jolt as default" revisited after J10's comparison? Recommendation: yes.
- **P6** Defer 2D (Box2D v3 with its own `Rigidbody2D`/collider set and seam) to a separate track started when a 2D game needs it and Box2D's snapshot is no longer experimental? Recommendation: defer.
- **P7** Strict float engine-wide (no fixed point), with engine-owned trig and a per-tick FP-environment guard? Recommendation: strict float.
- **P8** Character controller stays engine-owned over backend capsule casts/overlaps (netcode N9), with Jolt's `CharacterVirtual` only as a spike? Recommendation: engine-owned.
- **P9** Turn `N2ENGINE_STRICT_FP` on in the Windows Release, Linux clang x64/arm64 and web legs, add the cross-leg physics golden-hash job non-required, and make it required after two green weeks? Recommendation: yes.
- **P10** Parity bar for Jolt: the existing physics test suites run as typed tests on both backends, with any difference (single friction coefficient, friction combine, contact impulses) listed in docs and accepted by you in J3/J4? Recommendation: yes.
- **P11** Web: link Jolt into the web player by default only after J1's measured size is accepted (budgets raised deliberately, as PR #184 did), and later build web packages with only the project's backend (J10)? Recommendation: yes.
