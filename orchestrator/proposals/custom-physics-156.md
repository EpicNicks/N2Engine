# #156: a custom physics engine? Current state, options A-D, recommendation (2026-11-16)

Status: research and recommendation only. No engine code changed, nothing built or run for this report. Read at `master` 2b3b3a74.
Marks: **[V]** read in this repo (path) or in a vendor page fetched today (URL); **[M]** memory, unverified; **[E]** my estimate.

Related, not duplicated here: `physx-limitations-157.md` and `physx-determinism-157.md` (the #157 reports, which supersede the older `physx-limitations-and-determinism.md`; `physx-determinism-measured-numbers.md` holds the measured numbers). The earlier design this report builds on: `physics-backend-investigation-156.md` (2026-10-20, P1-P11 decisions), `physics2d-box2d-design.md`, `netcode-runtime-design.md`, `engine-stripping-design.md`.

## 0. Headline

**The investigation's question has been overtaken by the work.** Since the 2026-10-20 design (which recommended Jolt, deferred Box2D, and rejected a custom engine), the tree has gained both libraries and both backends [V]:

- **Jolt v5.6.0** vendored (`external/JoltPhysics`, MIT, always built `JPH_CROSS_PLATFORM_DETERMINISTIC`) and **`JoltBackend`** (`engine/src/physics/jolt/JoltBackend.cpp`, 4,026 lines) implementing the whole `IPhysicsBackend`: bodies, compound shapes, layers, triggers, events, raycast/raycast-all/sphere+capsule cast/overlap, set-state, **SaveState/RestoreState/HashState**, late-join handle claims. It declares all four `PhysicsFeature` groups and `SupportsSnapshot`/`SupportsSetState` [V `JoltBackend.hpp`].
- **A cross-leg CI proof**: a 10-scene corpus, 600 ticks, hashes every 60 ticks, on Windows MSVC, Linux clang x86_64, Linux clang arm64, Linux GCC 14 and web/node, compared with each other and with `tests/golden/physics-determinism.json` (122 records), no tolerance, with a self-testing comparer; all five legs equal in the J7 run [V `docs/physics.html` "Determinism proof in CI (stage J7)"]. The job was not yet a required check (plan: required after two green weeks) [V]; I did not verify whether that step was taken.
- **Box2D v3.2.0** vendored (`external/box2d`, MIT) and a **2D physics module** (Rigidbody2D, Box/Circle/PolygonCollider2D, triggers, events, queries, Lua) with upstream's golden and an engine-step state hash checked on every leg [V `docs/physics2d.html`, `tests/box2d/Box2DSmokeTests.cpp:17,108-118`].
- **Netcode on top**: lockstep relay, rollback (`RollbackWorld/Session/Runtime`), the Deterministic preset that refuses to start unless the backend is `CrossPlatform` + snapshot + all features [V `docs/physics.html` presets table; `docs/netcode.html` C3/C5].
- Backend selection per project (`physics.backend`), per-project module stripping (`build.modules.physics3d` none/physx/jolt/both, `physics2d`) so the web player can link only what it needs [V `PhysicsBackendKind.hpp`, `engine-stripping-design.md`, PR #315].
- Engine-owned deterministic trig exists (`math/include/math/DeterministicMath.hpp`, used by `Matrix.hpp`) [V].

**Recommendation: do not write a custom engine. Close #156 as "answered: option D, executed" (Jolt for deterministic 3D, Box2D for 2D, PhysX kept for the authoritative preset and GPU particles), and spend the remaining effort on four gaps (section 8): 2D in rollback/hash, measured snapshot and speed numbers, the thread-pool declaration, and a requirement on a CI "required" gate.** A custom engine stays a fallback with the exit criteria of section 9.

## 1. Use cases (deterministic gameplay primary)

| # | Use case | Needs | Served by (today) |
|---|---|---|---|
| U1 | **Lockstep / rollback online games** (primary, user) | Cross-platform bit-identical results on Windows, Linux x64/arm64, wasm; snapshot/restore; set-state; ordered events and queries; fixed step | Jolt 3D (all of it), Box2D 2D (determinism yes, rollback no, see 4.2) |
| U2 | **Web player (#155)** | Small wasm, single thread, no GPU, scalar | Jolt +978 KB raw / +272 KB brotli, Box2D +224 KB raw / +76 KB brotli in the size profile; PhysX +2.74 MB / +772 KB [V `engine-stripping-design.md` section 1, `web-build.html`] |
| U3 | **Replays, golden tests, server-side re-simulation** | Same-binary reproducibility | PhysX (same binary, measured bit-identical, `physx-determinism-measured-numbers.md`); Jolt |
| U4 | 2D / side-scrolling games | 2D bodies, triggers, queries, layers | Box2D module [V] |
| U5 | Character movement | Capsule cast, overlap, kinematic move | Engine-owned `CharacterController` over backend queries, typed tests on both backends [V `tests/engine/physics/CharacterControllerTests.cpp`] |
| U6 | Triggers, layers, events, ray queries | Feature parity | Both 3D backends [V] |
| U7 | Server-authoritative 3D (non-deterministic) | Set-state, GPU particles, big feature set | PhysX stays [V] |
| U8 | Not served by any backend today | Joints, CCD, convex hulls, triangle meshes, vehicles, ragdolls, destruction | Nothing exposed by the engine (neither backend's seam has them). Jolt and Box2D both have joints and CCD upstream [V Box2D/Jolt READMEs per the 10-20 report]; PhysX has everything |

The maintainer's clarification (2026-10-15): "specialised" means anywhere PhysX does not make sense: the web player and determinism-critical play [V issue comment]. Both are covered.

## 2. What the Jolt and Box2D backends cover (state)

### 2.1 JoltBackend (3D) [V `JoltBackend.hpp`, `docs/physics.html`]
Covered: static/dynamic/kinematic bodies; ONE Jolt shape per body (compound of all colliders, triggers included as sensor sub-shapes); sphere/box/capsule only; mass from solid colliders; gravity flag; forces/impulses/velocities; enable/disable (parking); 32 layers and the matrix with re-filter semantics; Enter/Stay/Exit events through the shared `ContactTracker`; queries via `NarrowPhaseQuery` only with engine-side sorting (`QueryUtil`, tie tolerance 0.001); snapshot = Jolt `SaveState(All)` + backend bookkeeping (colliders, mass, matrix, gravity, contact map, undelivered events), checksum and version, atomic refusal; `HashState` (canonical, handle order) and `HashSimulatedState` (order-independent session hash); handle claims for late joiners; body ids = engine handles via `CreateBodyWithID`; single-threaded job system on web and when `physics.deterministic` is on.

Not covered: joints/constraints, meshes, hulls, height fields, CCD, soft bodies, Jolt's own character controllers, GPU anything, static friction, exact contact impulses (Jolt reports before the solver: `normalImpulse` is an estimate, `tangentImpulse` 0 on both backends; "a game that needs the exact applied impulse needs PhysX") [V].

Known behavioural differences vs PhysX (accepted in the typed tests, margins 0.06 m height / 0.05 m/s): single friction coefficient per body with per-contact override, solver iterations (10 vel / 2 pos vs 4/1), sleeping thresholds, contact tolerance, body limit 10,240, side-A choice in pairs [V physics.html table].

Declaration rule [V]: `None` with the thread pool; `SameBinary` single-threaded without the strict profile; `CrossPlatform` only with single-threaded job system + Jolt CPD + `N2ENGINE_STRICT_FP`. Strict FP is on for the Windows Release, linux clang x64/arm64, GCC and web dev legs, and by default in the web size/speed profiles and the container image since PR #276.

### 2.2 Box2D module (2D) [V `docs/physics2d.html`, `physics2d-box2d-design.md`]
Covered: components, one world per scene, fixed step with 3D, events with the 3D rules, shared 32-layer matrix, ray/circle/box casts and overlaps with `QueryRules`, Lua, serialization, strippable (`N2ENGINE_USE_BOX2D`), built single precision with contraction off and (web) `BOX2D_DISABLE_SIMD`, single worker. Determinism: upstream Falling Hinges golden plus an engine-step hash on every leg [V].
**Not covered: any hook into rollback or the desync hash.** Design decision H: 2D is allowed in `SimulationMode::Deterministic` with one warning; "hashing and rollback are follow-ups" and a rollback session "would rewind 3D and game state but not the 2D world, and would be wrong with no error" [V `physics2d-box2d-design.md:95-97,115`; grep of `engine/src/network` finds no 2D reference]. There is no `IPhysics2DBackend` seam (Physics2D is concrete on Box2D).

### 2.3 PhysX
Unchanged: default, same-binary determinism only, no snapshot; see the #157 reports.

## 3. Options A-D (plus E: Jolt/Box2D as the deterministic backends)

Sizes are web size-profile deltas from the repo's own CI numbers where marked [V], else [E].

| Option | What | Effort | Risk | wasm cost | Deterministic gameplay | Status |
|---|---|---|---|---|---|---|
| **A. PhysX only** (+ PhysX on wasm) | Native PhysX, `physx-` web build | Done (PR #184) | Cannot meet cross-platform determinism: Windows vs Linux differ even for free fall, web differs from both [V measured-numbers] | +2.74 MB raw / +772 KB brotli [V] | No | Built; stays for U3, U7 |
| **B. Custom lightweight engine** | Own broadphase/narrowphase/solver behind the seam | 25-40 PRs, 4-8 months, 9-14k lines (box-box SAT, stacking tuning, sleeping, CCD tail) [E from the 10-20 report; I re-derived none of it] | High: quality tail, ongoing tuning; fixed-point variant +40-60 % [E] | Probably 0.2-0.5 MB raw [E] | Yes, by construction, plus trivially copyable state | Not started; not needed |
| **C. Hybrid** custom + PhysX | B for web/deterministic | B plus coexistence | B plus two behaviours | As B | Yes | Superseded: the hybrid is now PhysX + Jolt |
| **D. Reuse OSS engine** | Jolt (3D), Box2D (2D); Rapier, Bullet etc. rejected | Done: J1-J7, B5, Physics2D 1-3 | Low-medium: relies on upstream's claim plus our CI | Jolt +0.98 MB raw / +272 KB brotli; Box2D +224 KB raw / +76 KB brotli; with module stripping a no-physics player is 4.4 MB raw vs 8.4 MB [V] | **Yes, 3D proven on 5 legs for the corpus; 2D proven for hash, not rollback** | **Built** |
| **E. = D, as the deterministic backends** | Deterministic preset forces Jolt (3D); 2D pending | Remaining: section 8 | | | | Chosen and executed |

Why not Rapier [V rapier.rs/docs/user_guides/rust/determinism, fetched today]: `enhanced-determinism` gives cross-platform results on IEEE-754 targets, incompatible with `simd8`, parallel equals sequential; WASM covered implicitly. Rejected for the Rust toolchain on five legs and no C++ API [M], not for determinism.

## 4. Verified external claims

| Claim | Result |
|---|---|
| Jolt `CROSS_PLATFORM_DETERMINISTIC`: ~8 % slower, deterministic regardless of compiler, configuration, OS, architecture, word size | Confirmed [V Architecture.md, jrouwe/JoltPhysics master, fetched 2026-11-16] |
| Jolt verified legs include Windows MSVC, Linux clang/gcc x86 and ARM, WASM under node | Confirmed [V same] |
| Jolt thread-count independence | **Not stated** in that section; callbacks run multi-threaded in no fixed order; broadphase queries not deterministic; `GetActiveBodies` order nondeterministic [V same]. Our backend works around each (single-thread job system by default, narrowphase queries only, sorted buffers, handle-ordered sync) and has one-scene 0 vs 1/2/4 thread test [V physics.html] |
| Jolt rollback via `SaveState/RestoreState`, bodies added/removed in the window break restore, `CreateBodyWithID`, filter to skip static bodies | Confirmed [V same]; our "parked body" contract implements it |
| Box2D v3 cross-platform determinism (x64/ARM; MSVC/GCC/Clang; no fast-math, FMA off, own trig, multithread-stable) | Confirmed from the 2024 blog [V]; the blog says Box2D "does not provide roll-back determinism" and does not mention wasm. v3.2.0 (2026-10-19) added experimental `b2World_GetSnapshot` with an explicitly unstable format [V 10-20 report reading `box2d.h`; I did not re-read it today] |
| Box2D on wasm | Not claimed upstream; our own CI compares the hash on the web leg [V `physics2d-box2d-design.md:22`, `physics.html`] |
| Rapier enhanced determinism | Confirmed [V] (above) |
| Wasm floating point determinism | Basic IEEE ops deterministic by spec except NaN payloads and relaxed-SIMD [M; asserted in `web-build.html` per the 10-20 report, not re-checked against the spec today] |
| Unity: PhysX/Box2D not deterministic across machines; DOTS Unity Physics stateless for rollback; Havok stateful and not cross-platform | **Not verified** (maintainer asked; memory only) |
| Who ships Jolt deterministically in lockstep/rollback | **Not verified** |

## 5. IPhysicsBackend interface: what was needed vs what exists

All of the 10-20 list is present in `IPhysicsBackend.hpp` [V]: `GetDeterminism`, `SupportsSnapshot`, `SupportsSetState`, `GetImplementedFeatures`, `BackendId`, `GetStats`, `GetBodyState/SetBodyState`, `SaveState/RestoreState`, `ClaimBodyHandles`, `HashState`, `HashSimulatedState`, `CapsuleCast`, `Overlap`. Contract documented: main thread, between steps; QueryRules (ordered by handle, start-inside = distance 0).

Remaining interface items, in value order:
1. **A 2D seam** (`IPhysics2DBackend`, or at least `SaveState/RestoreState/HashState` on `Physics2DWorld`) so `RollbackWorld` and `StateHash` can include 2D. Box2D's snapshot is experimental, so a custom serializer over `b2World` state is an alternative: [M] unknown whether Box2D exposes enough to save/restore the contact cache without the experimental API.
2. **Fixed-point / own math: not needed** for Jolt/Box2D (both strict float). The interface stays `Math::Vector3` float. Fixed point would only come with option B.
3. **Ordered iteration**: already a contract (handles, sorted events, sorted queries). Add the Box2D equivalents when 2D joins.
4. A `SnapshotCost` stat on `GetStats` to make the cost visible in the Profiler (new, optional).
5. Joints/CCD/mesh colliders are a seam extension, not a determinism need; only if a game needs them (decision C below).

## 6. Integration (Rigidbody, Collider, Lua, editor) [V]

- Rigidbody, ICollider and Raycast reach physics only through `Application::Get3DPhysicsBackend()`; the Jolt backend is a drop-in. Lua `Physics.*` and the character controller are typed-tested on both backends.
- Editor: backend picker through `GetPhysicsBackends`/`physics.backend` (protocol 1.20.0, J9); Network panel Deterministic card driven by the backend declaration. Live-editor verification of the picker is still a user item (CONTINUE.md "live-host run").
- Backend is per project (set at Init), not per scene. Deterministic preset never falls back to another backend; it refuses (D4).
- Behaviour users will see: section 2.1 differences; 2D differs from 3D in friction combine (geometric mean vs average), dynamic-body transform writes teleport in 2D but are ignored in 3D [V physics2d.html].
- Soft bodies and particles: CPU backends on Jolt, as on web and Linux.

## 7. Size and performance

- Size: see option D row; budgets in `web/budgets.json` already raised for both. Stripping measured a no-physics size-profile player at 866 KB brotli vs 1,996 KB with all three [V `engine-stripping-design.md`].
- Native speed: Jolt CPD costs ~8 % by upstream's claim [V]. **No Jolt-vs-PhysX measurement exists** (listed as not measured in `docs/physics.html`; J10's report was never done) [V]. Existing web number: PhysX 0.81 ms/step for 200 boxes under node dev profile (from the 10-20 report, not re-read).
- Targets proposed [E]: 300 dynamic bodies at 60 Hz under 2 ms/step native, under 6 ms on web speed profile; `SaveState` + `RestoreState` pair under 0.5 ms native / 2 ms web for 300 bodies (the 10-20 criterion 6.4(b)); measure with the corpus scenes plus a 300-body pile, reporting ns/body.
- RestoreState first takes a safety snapshot so a restore costs about 2x a save [V physics.html]; if the measured pair misses the target, a flag to skip the safety snapshot is the cheapest fix.

## 8. Remaining work, PR-sized, with confidence

| PR | Content | Size | Confidence it lands as described |
|---|---|---|---|
| R1 | Benchmark + report (no code behaviour): Jolt vs PhysX on corpus scenes and a 300-body pile, native + web node; snapshot save/restore times and sizes; prints in CI like `[BOX2D-SIZE]` | S-M | High (85 %) |
| R2 | Make the `determinism` job a required check after the green-weeks window; add a Windows Debug note; a nightly run over random scene seeds (fuzz corpus) to widen beyond 10 fixed scenes | S | High |
| R3 | Corpus widening: mesh-free but heavier scenes (500 bodies, long 5,000-tick run, rollback with 15-tick window, spawn/despawn under parking, thread pool 0 vs 4 with 150+ bodies). Decide whether `DeclaredDeterminism(false)` can become `CrossPlatform` with the pool | M | Medium (60 %): upstream does not document thread-count independence, so a mismatch is possible and would have to go upstream |
| R4 | 2D hash: `b2World_GetStateHash`-style hash into `StateHash` and a desync entry for 2D; remove the "wrong with no error" warning once covered | S-M | High (80 %) |
| R5 | 2D rollback: world save/restore for Box2D (experimental snapshot or own serializer), parked-body contract for 2D, corpus scenes 2D on 5 legs | L | Medium-low (45 %): upstream says the snapshot format "will change"; pin v3.2.0 and re-baseline per bump |
| R6 | Optional seam work for games that need them: joints (hinge/fixed first), CCD flag, convex hull/mesh colliders on Jolt and PhysX, each with parity tests | L per feature | Medium; only on a stated game need |
| R7 | Docs: close #156 with a link to this file; update `physics.html` "Not verified" once R1 and R3 report | S | High |

Minimal prototype (the issue asks for a throwaway box stack + raycast behind the seam): **not worth building**. `PhysicsCoreTests`/`PhysicsQueryTests` already run a box stack, raycasts and more on both backends, and a throwaway custom backend would calibrate option B only. If the maintainer still wants the estimate calibrated, the cheapest honest prototype is a 3-day sphere/plane/box-vs-plane solver behind the seam (about 1,500 lines [E]) measuring how far it gets on `PhysicsCoreTests` free fall, floor rest and friction ordering; it would tell little about box-box stacking.

## 9. Hard parts, risks, and when a custom engine would be revived

Risks now live in what we own and in upstream:
1. **Glue nondeterminism** (a `std::sort` with ties, an unordered iteration, engine SIMD math in a conversion): mitigated by the corpus and the comparer self-test; the corpus is 10 scenes and "does not show equality for other scenes, the thread pool, the web speed/size profile builds (built without tests), the Windows Debug legs, or other CPUs" [V physics.html].
2. **Toolchain bumps** (MSVC, clang-20, emscripten 4.0.10) can change results; Box2D itself says "determinism is a brittle feature" [V blog]; the golden makes this visible, not impossible.
3. **Thread count**: unproven upstream; the Deterministic preset stays single-threaded (cost: parallel speed-up on native).
4. **Impulse and friction semantics** differ from PhysX (accepted).
5. **FP environment** (FTZ/DAZ set by another library) [M]; I did not confirm that a per-tick guard exists in the engine; verify in R3.
6. **Lua**: numbers are doubles; `math.*` shims and `pairs` order were B4/B8 items in the netcode design; I did not re-verify their state today.
7. **Jolt bus factor** (one principal maintainer [M]); mitigated by MIT, vendored pin and SHA256 check script.
8. **2D rollback** (section 2.2) is the one real hole against the primary use case for 2D deterministic games.
9. Years of PhysX tuning we would not match: moot, PhysX stays.

Revive option B if (a) a cross-leg mismatch inside Jolt/Box2D is not fixed upstream in ~4 weeks, (b) snapshot cost makes the rollback window unaffordable (thresholds in section 7), (c) a feature the games need (e.g. exact impulses, mixed trigger bodies) cannot be worked around, (d) upstream is abandoned or relicensed, or (e) a target without IEEE-754 conformance appears (then fixed point). None has occurred [V as far as the docs report].

## 10. Verification strategy

Already in place [V]: typed backend-agnostic tests (`PhysicsCoreTest/PhysX` and `/Jolt`, layers, order, events, queries, body state, character controller); a corpus of ten scenes with committed golden hashes on five legs; PhysX control hashes expected to differ (proves the job can see a difference); comparer self-test; snapshot round-trip and rollback equality tests; thread-count equality for one scene; Box2D upstream golden and engine golden on every leg; `JoltBackendDeterminismTest` constants.
To add: R1-R5; cross-backend comparison against PhysX stays within the typed-test margins only (no bit comparison, by design); a native-vs-WASM equality leg for the speed profile (needs a tests-on build of that profile, currently skipped as an extra full build) [V physics.html "decision (4)"].

## 11. Licence notes

Jolt MIT (Jorrit Rouwe), Box2D MIT (Erin Catto), both in `THIRD_PARTY_NOTICES` with the unmodified `LICENSE` vendored [V]. Notice text that says Jolt is "not linked into any engine, player or editor target yet" is stale (the backend links it; `THIRD_PARTY_NOTICES` line 29): fix in R7 [V]. PhysX BSD-3 unchanged. Porting algorithms from Jolt (e.g. its trig) is MIT-compatible with a notice; the engine already wrote its own `DeterministicMath`.

## 12. Decisions needed from the maintainer

1. **Close #156** as answered by D (Jolt + Box2D), with a custom engine only on the section 9 triggers? Recommended: yes.
2. **2D deterministic play (R4/R5)**: is deterministic 2D a real near-term goal? If yes, accept R4 now and R5 with its re-baseline risk; if no, keep the warning and document. Recommended: R4 now, R5 when a 2D online game exists.
3. **Joints/CCD/meshes (R6)**: any game need to name? Without one, none.
4. **Make `determinism` required** (R2) and widen the corpus (R3), accepting more CI time? Recommended: yes.
5. **Thread pool for the Deterministic preset**: keep single-threaded (safe, slower) until R3 shows equality, or accept upstream's silence? Recommended: keep single-threaded.
6. **Perf bar**: accept the section 7 targets, and the policy that Jolt becomes the default backend only if R1 shows parity on the corpus (today PhysX is the default)?
7. Provide or approve checking the unverified claims about Unity DOTS/Havok and shipped lockstep Jolt titles, or drop them from the docs.

## Could not verify
Unity/Havok claims; Jolt in a shipped lockstep game; Jolt thread-count independence (upstream silent); Box2D snapshot API today (read on 10-20 only); wasm determinism from the spec text; whether `determinism` is already a required check; the FP-environment guard and Lua shims state; any performance or snapshot-cost numbers (none exist in the tree).
