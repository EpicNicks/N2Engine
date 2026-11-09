# Physics2D with Box2D v3: design

Items: `orchestrator/items/box2d-p1-vendor.json`, `box2d-p2-module.json`, `box2d-p3-lua-docs.json` (split from
`box2d-2d-physics.json`, which keeps the original brief).
Written 2026-11-08 against `master` at `34de33b6`, with upstream Box2D at tag `v3.2.0`
(commit `178495ba35695aefe9c321e569d2277f06db3b90`).

**Status: approved by the user on 2026-11-08, with every recommendation in section 4 accepted:** G `OnCollisionEnter2D`
names, H warn once in Deterministic mode, I three PRs. The same day the user also asked for stripping, which adds section 13.

## 1. Summary

Add a separate 2D physics module on Box2D v3.2.0, vendored like Jolt, with Unity-style components
(`Rigidbody2D`, `BoxCollider2D`, `CircleCollider2D`, `PolygonCollider2D`), one Box2D world per loaded scene and the
existing 32 layers and collision matrix. It steps in the fixed step after the 3D backend. Game code receives events
only after the step, when Box2D is unlocked.

Two things were measured rather than assumed, because the item treats them as gates (section 2):

- **Size:** Box2D alone is about 64 KB brotli in the size profile, roughly a third of the headroom. The wrapper and Lua
  bindings still have to be measured. No size or speed budget should need raising, but the dev budget on master does.
- **Determinism:** upstream's own golden test reproduced on wasm (with and without SIMD) and on native x86. Upstream
  does not claim wasm; this is the first evidence for it.

Three things in the item do not work as written. They are in section 3 and need a decision or an acknowledgement.

## 2. What was measured

A throwaway probe outside the repository, built with Emscripten 4.0.10 (the version CI pins) and with GCC under
w64devkit. It is not MSVC, not arm64 and not the engine's real build, so CI remains the proof.

### Determinism

Upstream's `Falling Hinges` scene (`shared/determinism.c`, driven as `test/test_determinism.c` drives it), then the same
scene at the engine's 50 Hz step, hashed with `b2World_GetStateHash` after 100 steps while 80 bodies were still moving.

| Build | Hash at sleep | Sleep step | Upstream golden | 50 Hz state hash, mid-flight |
| --- | --- | --- | --- | --- |
| wasm, `-O2`, no SIMD | `0x465F3DF4` | 299 | match | `0x382FECF9034E1157` |
| wasm, `-O2`, simd128 | `0x465F3DF4` | 299 | match | `0x382FECF9034E1157` |
| wasm, `-Oz -flto`, no SIMD | `0x465F3DF4` | 299 | match | `0x382FECF9034E1157` |
| native GCC, `-mavx2` | `0x465F3DF4` | 299 | match | `0x382FECF9034E1157` |
| native GCC, SSE2 | `0x465F3DF4` | 299 | match | `0x382FECF9034E1157` |

This one scene is evidence, not a guarantee. MSVC and arm64 were not run; section 9 adds them through CI. The two native
rows are not independent of each other: Box2D picks its solver width at runtime with cpuid (`b2GetSIMDWidth`), so both may
have run the same path on this CPU. The real comparison is native against wasm.

### Size

Box2D alone, reaching every API this module plans to use, minus a libc-only baseline with the same flags. The profile
flags are those of `cmake/N2EngineWeb.cmake`. Headroom comes from the web job of master run `42449005393`.

| Profile | Measure | Box2D delta | Master measured | Limit | Headroom | Share of headroom used |
| --- | --- | --- | --- | --- | --- | --- |
| size | wasm raw | 168,629 | 8,136,557 | 8,900,000 | 763,443 | 22% |
| size | wasm brotli | 63,853 | 1,901,099 | 2,090,000 | 188,901 | 34% |
| size | wasm gzip | 76,522 | 2,700,986 | 2,970,000 | 269,014 | 28% |
| size | initial brotli | 63,853 | 1,966,831 | 2,150,000 | 183,169 | 35% |
| speed | wasm raw | 244,492 | 14,524,126 | 15,600,000 | 1,075,874 | 23% |
| speed | wasm brotli | 76,969 | 2,217,012 | 2,400,000 | 182,988 | 42% |
| speed | wasm gzip | 97,508 | 3,510,254 | 3,790,000 | 279,746 | 35% |

The initial-brotli delta reuses the wasm number, since the probe has no packaged files to add. These figures do **not**
include the C++ wrapper or the sol2 Lua bindings, which are template-heavy and may be significant. The PR measures the
real total and records it, as the Jolt PR did.

**Dev profile:** master's `wasm.raw` is 293,409,915 of 294,000,000, leaving 590,085 bytes, and dev builds carry debug
info. PR #311 raises that limit to 340,000,000. If this work lands after #311 it fits; if it lands first it needs the
same deliberate raise. Recommendation: branch after #311 merges.

### Behaviour of Box2D that the contracts depend on

A native run of the exact behaviours (begin/end counts over one pass-through):

| Pair | Contact | Sensor | Consequence for the design |
| --- | --- | --- | --- |
| dynamic solid to static or kinematic solid | 1/0 (it stops) | 0/0 | Matches the 3D table |
| kinematic solid to static or kinematic solid | 0/0 | 0/0 | No event, as the 3D docs' "at least one dynamic" |
| dynamic or kinematic solid through static sensor | 0/0 | 1/1 | Trigger events work, including for kinematic bodies |
| dynamic or kinematic sensor through static solid | 0/0 | 1/1 | Works |
| sensor through sensor | 0/0 | **2/2** | **Differs from 3D: the wrapper must drop these** |

Also confirmed: queries see sensors (ray, overlap and shape cast), which matches the 3D default of hitting triggers. A
matrix-disallowed pair gives no contact, no sensor event and passes through. Contact data is readable at begin touch.
A **ray starting inside a box does not hit**, which breaks `QueryRules`; section 7 handles it.

## 3. Where the item does not work as written

1. **Event names (decision).** The item says `OnCollisionEnter` and friends. The existing virtuals take
   `const Physics::Collision &`, whose fields are `Physics::Rigidbody *` and `Physics::ICollider *`. A 2D body is neither
   and cannot be without inheriting 3D members. See decision G.
2. **Trigger Stay (acknowledgement).** Scope item 4 lists trigger Enter and Exit; the acceptance list says Enter, Stay
   and Exit, and 3D has Stay. This design implements Stay, the larger reading.
3. **"A Deterministic project can include it" (decision).** `StateHash.cpp` and `RollbackWorld` only read
   `IPhysicsBackend`, and networking is out of scope, so 2D state is in neither the desync hash nor a rollback. A
   rollback session would rewind 3D and game state but not the 2D world, and would be wrong with no error. See decision H.

One point differs from the reviewer note in the item, which says destroy and create are deferred. Section 6 explains
why they run immediately, and why that is still safe.

## 4. Decisions

All accepted by the user on 2026-11-08. A to F are the item's; G to I come from this design.

| | Decision | Recommendation | Why |
| --- | --- | --- | --- |
| A | Box2D version | **v3.2.0** (requested by the user) | Current release. `b2World_GetStateHash` and continuous sensor events are in it |
| B | Module shape | **Separate module with a concrete `Physics2DWorld`, no `IPhysics2DBackend`** | One backend exists and is planned. Box2D runs headless and is fast, so tests use the real thing instead of mocks. An interface can be added if a second backend ever appears |
| C | Components | **`Rigidbody2D`, `BoxCollider2D`, `CircleCollider2D`, `PolygonCollider2D`** | Mirrors 3D, and the game being ported comes from Unity |
| D | Layers | **Same 32 layers and same matrix** | Section 5 shows how it cannot drift |
| E | Plane and units | **World x, y and rotation about z. z is untouched. 1 unit = 1 m, no knob** | `b2SetLengthUnitsPerMeter` is process-global, so it would be a trap. Sizes are unscaled, as for 3D colliders |
| F | Determinism | **Engine fixed step, `workerCount = 1`, no task callbacks, no wall-clock** | `b2GetTicks` in the step feeds only the profile struct. With one worker Box2D creates no threads |
| G | Event names | **`OnCollisionEnter2D`, `Stay2D`, `Exit2D` and `OnTriggerEnter2D`, `Stay2D`, `Exit2D`, taking `Collision2D` and `Trigger2D`, in C++ and Lua** | Same order and field names as 3D, and no ambiguity on an object using both worlds. Reusing the names would mean overloads that hide each other in C++ and a Lua handler whose argument changes type |
| H | 2D under network modes | **Allowed, with one warning when a 2D world is created in `SimulationMode::Deterministic`, plus a docs section. Hashing and rollback are follow-ups** | Warns about a silent failure without pulling networking into this item. Refusing to start would be stricter but blocks offline-tested lockstep experiments. Triangle-Like is unaffected |
| I | PR split | **Three PRs: vendor and build; module, events, queries, serialization; Lua and final docs** | The vendored tree is large and hard to review mixed with code, which is why Jolt's J1 was its own item. This needs the item split into three, as Jolt's was |

## 5. Structure

### Vendoring and build

- `external/box2d/`: `src/`, `include/`, `LICENSE`, `shared/determinism.{c,h}` and `VENDORED.md` (tag, SHA, date, what
  was pruned, a hash list). Byte-identical to upstream, no patches. `samples`, `test`, `benchmark`, `docs` and `extern`
  stay out. The two `shared` files let the upstream golden run on every leg.
- `cmake/box2d/CMakeLists.txt`: our own target `box2d`, alias `box2d::box2d`, so every option has one fixed value, as in
  `cmake/jolt`. Upstream's CMake is not used because it turns wasm SIMD on for Emscripten and adds samples, install and
  packaging rules the engine does not need.
  C17, double precision off, `BOX2D_VALIDATE` off, warnings not fatal, include directories marked `SYSTEM`.
- `-ffp-contract=off` on the target (`/clang:-ffp-contract=off` for clang-cl), and also on the wrapper's source files,
  because Box2D's math is inline in its headers and is compiled into them.
- `contact_solver_w8.c` keeps its per-file AVX2 flag so upstream's runtime dispatch works. The engine's AVX floor is
  unaffected, because the file only runs on a CPU that reports AVX2.
- **Web: `BOX2D_DISABLE_SIMD`.** Upstream's CMake enables `-msimd128`, but the player has wasm SIMD off (`USE_WASM_SIMD
  OFF` for Jolt), so it would add a new browser requirement. Measured cost of staying off: about 10 KB raw and 5 KB
  brotli, with an identical hash. The performance cost was not measured.
- `N2ENGINE_USE_BOX2D`, default ON everywhere: no binaries, no network. With OFF the components still compile and
  register, and their calls do nothing but warn once, so a scene keeps its data and the registry and its network
  fingerprint do not change with the option. This mirrors "physics components cope with a null backend".

### Code layout

Under `engine/include/engine/physics2d/` and `engine/src/physics2d/`, namespace `N2Engine::Physics2D`. Box2D types stay
out of the public headers.

| Piece | Role |
| --- | --- |
| `Physics2DWorld` | Owns the `b2WorldId`, pending changes, the step, sync, events, queries |
| `Rigidbody2D` | Body type, velocities, damping, gravity scale, fixed rotation, bullet, mass |
| `Collider2D` (base), `Box`, `Circle`, `Polygon` | Shapes, offset, trigger flag, material, density, surface speed |
| `ContactTracker2D` | Shape-pair to body-pair events (section 6) |
| `Physics2DTypes` | `Collision2D`, `ContactPoint2D`, `Trigger2D`, `RaycastHit2D`, `OverlapHit2D` |
| `Physics2D` (static API) | Queries on the current scene's world |

### Who owns the world

`Scene` owns a `std::unique_ptr<Physics2DWorld>`, created when the first 2D component attaches, so a 3D scene has no
world and no cost. It is destroyed after `Scene::Clear` destroys the components.

- `SceneManager` has one loaded scene, rebuilt from data on load, so a scene switch tears everything down cleanly.
- The prefab stage parks a scene with `SwapLoadedScene`. Separate worlds mean stage bodies and parked bodies cannot
  meet, and only the scene passed to `PhysicsUpdate` steps.
- Box2D allows 128 worlds, which is far above what parked scenes can reach.
- An object moved to another scene gets a new body through the existing re-attach path. Its velocity is not carried over.

### Fixed step

The 3D sequence keeps its calls and their order. With no 2D world, every added line is a null check.

```text
3D ApplyPendingChanges
2D ApplyPendingChanges
scene.FixedUpdate()
3D Update, SyncTransforms, ProcessCollisionCallbacks
2D Step, SyncTransforms, DispatchEvents    <- world looked up here: FixedUpdate may have created it
```

The `else` branch (no 3D backend, which also covers builds with neither backend) runs the same 2D lines. A
no-backend project must still get 2D physics. 2D time counts toward the frame record's `physicsMs`.

### Layers: why the matrix cannot drift

- Each shape: `categoryBits = 1 << layer`, `maskBits = Layers::CollisionMaskFor(layer) | kQueryBit`. Box2D requires both
  sides to allow a pair, the same symmetric rule as the matrix. Sensor events use the same check.
- `kQueryBit` is bit 32, outside the 32 layers. Box2D's query filter also requires the shape's mask to accept the query's
  category, so without it the collision matrix would change query results. Queries use category `kQueryBit` and mask
  `layerMask`. Measured: results follow the layer mask, and an empty mask finds nothing.
- `Layers::NotifyMatrixChanged` also refreshes the live 2D worlds; `GameObject::SetLayer` also tells 2D colliders. Each
  shape gets `b2Shape_SetFilter`. There is no second copy of the matrix anywhere.
- No custom filter callback, which would run inside the step.

### Transforms

- **Engine to Box2D:** a change marks the body dirty through `Positionable::NotifyPhysicsComponents`, and it is applied
  in 2D `ApplyPendingChanges`, as `docs/application.html` promises. Kinematic bodies use `b2Body_SetTargetTransform`, so
  they carry what stands on them. Static and dynamic bodies use `b2Body_SetTransform`, a teleport.
- **Box2D to engine:** `b2World_GetBodyEvents` lists only what moved. Position x and y are written, z is kept, rotation
  becomes a pure z rotation, and the write does not mark the body dirty again.
- **Known effects, documented:** Box2D ends contacts on a teleport, so a teleported touching body gets Exit and then
  Enter. X or Y tilt on a rotating 2D body is cleared at the first sync with one warning; `Sprite2D` flips cover the
  usual reason for tilting. There is no render interpolation, the same as 3D.

### Bodies and shapes

- Mirrors 3D: a collider with no `Rigidbody2D` gets its own static body, and shapes are per collider so one body can mix
  solid and trigger. A disabled or inactive object is out of the simulation (`b2Body_Disable`). Colliders use only their
  own object's Rigidbody.
- **`SetIsTrigger` re-creates the shape.** Box2D states a shape cannot start or stop being a sensor. Pairs end with Exit
  and start again with Enter if still overlapping.
- **Mass: from shapes by default**, as the item asks, using a per-collider density (default 1). `SetMass` overrides it
  and rescales inertia. This differs from 3D, where mass is always authoritative; `_useAutoMass` records which applies.
- **Triggers have density 0**, so they add no mass, the same rule as 3D. A dynamic body with only triggers would have
  zero mass, so it gets mass 1; a test proves it responds to forces.
- `PhysicsMaterial` is reused: `dynamicFriction` and `restitution` map across, and `staticFriction` is unused since
  Box2D has one coefficient. Box2D's own combine rules stay, unlike the 3D average, and the docs' differences table says so.
- **Conveyors:** `SetSurfaceSpeed` on a collider maps to `b2SurfaceMaterial::tangentSpeed`. It is one field and the game
  needs it.
- **Polygon limits:** at most 8 vertices (`B2_MAX_POLYGON_VERTICES`), convexified by `b2ComputeHull`. Bad input logs a
  warning and creates no shape.
- **Polygon vertices in the inspector:** `FieldKind` has no list of `Vector2`, and a new kind needs a protocol minor. So
  vertices serialize in scenes and prefabs but are not editable in the inspector yet. Code, Lua and the Unity converter
  can set them. The implementation confirms the schema tolerates the field.

## 6. Events

- **Nothing runs during the step.** No pre-solve or custom-filter callback is registered, so no engine or game code can
  run while Box2D is locked.
- **Copy, then dispatch.** After the step, contact and sensor events are copied into engine vectors and sorted by handle.
  Box2D's arrays can go invalid when shapes are destroyed, so user code never sees them.
- `ContactTracker2D` applies the same rules as 3D: Enter on a body pair's first touching shape pair, Exit on the last,
  Stay every step while touching including the step of Enter. Order: forgotten-pair Exit, collision Enter, Stay, Exit,
  then trigger Enter, Stay, Exit. It iterates ordered maps, so the order does not depend on hashing or addresses.
- **Sensor-vs-sensor pairs are dropped**, per the measurement.
- **Why not reuse `ContactTracker`:** it is written against `Rigidbody`, `ICollider` and `Collision`, so sharing it means
  templating 557 lines of `ContactTracker.cpp` that both 3D backends rely on, and the item says the 3D path stays
  untouched. The cost is parallel code; a shared test table runs the 3D scenario list against 2D to keep them aligned.
  Merging them could be a later cleanup.
- `Component` gets six new empty virtuals. `LuaComponent` gets six new handlers. That is the only shared-class change
  for events.

**Create and destroy inside a callback run immediately instead of being queued.** The world is unlocked and events are
already copied, so it is safe, and each delivery re-checks that both sides are still alive. A queue would make
`rb:SetVelocity()` right after creating a body silently do nothing inside a callback. `GameObject::Destroy` stays
deferred to the end-of-frame purge as it is today. Destroy-in-callback tests prove this, and the reviewer note in the
item should be read with this in mind.

**Contact data:** normals are flipped per receiver to match "from other to this". Velocities are already post-solve at
dispatch, so `relativeVelocity` is not the approach speed. For a mirror, use restitution 1 and friction 0 on the shapes
rather than reflecting by hand.

**Fast projectiles:** per the v3.2 notes, fast shapes are swept against static sensors and bullets against non-bullet
sensors, but a moving *sensor* itself may miss. Projectiles should be solid `bullet` bodies filtered by layer, or use
shape casts. `docs/physics2d.html` says this, as it affects how the game's guns are built.

## 7. Queries

`Physics2D.Raycast`, `RaycastAll`, `OverlapCircle`, `OverlapBox`, `CircleCast`, `BoxCast`, with `layerMask` and
`QueryTriggers`. `QueryRules` from `Raycast.hpp` is reused as is: degenerate input finds nothing, the same distance
clamps and tie tolerance, ties and lists ordered by body handle.

- **A start inside a shape must hit at distance 0 with normal `-direction`.** Measured: Box2D's ray returns no hit from
  inside a box. So a ray first does a point overlap at its origin and adds those hits, and shape casts get the same rule
  tested.
- `QueryTriggers::Ignore` is a check in the result callback. Measured: queries see sensors by default.
- Results are collected, then sorted, so tree order never leaks out.

## 8. Lua, serialization, editor

- **Lua:** components through `BindComponentType` and `ComponentRef`, the same checked handles as 3D, so a stale handle is
  a checked failure and never a dangling pointer. A new `Physics2D` table, and `CollisionToLua`-style converters. `engine-api.lua` is updated.
- **Serialization:** `RegisterMember` and `BuiltinComponents` give scenes, prefabs and overrides without new machinery.
  Tests prove the round trip and an override.
- **Editor: no protocol bump.** `HandleGetComponentTypes` lists the registry dynamically, so the new components appear
  in the Add Component list and inspector on their own. Nothing in `tests/protocol`, `tests/editor` or
  `tests/engine/network` pins the type list. No health row is added.
- **Network fingerprint:** `HashRegistry` hashes type names, so it changes with this build. That is expected, because
  peers must already run the same build, and the tests compare hashes relatively with no golden to update.

## 9. Tests and CI

GoogleTest, with Box2D headless and no window:

- Bodies, each shape, sensors, layer filtering including a matrix change mid-touch, and every row of the section 2 table.
- Event order, per-body-pair behaviour on compound bodies, trigger toggling, destroy and create in a callback, scene
  reload, prefab stage swap, no-3D-backend stepping, and transform sync both ways.
- Queries against `QueryRules`, including starting inside a shape and an empty mask.
- Serialization round trip and prefab override; Lua behaviour including a stale handle.
- **Determinism:** two runs give the same hash; the upstream Falling Hinges golden and an engine-scene golden on **every
  leg, including web under node**. This is a hard-coded constant, like the Jolt smoke test, so a difference fails CI.
  That extends the item's "if the golden job is extended", because this test is cheap and is the only proof for MSVC and arm64.

3D physics tests are not edited. `N2ENGINE_USE_BOX2D=OFF` gets one engine-only compile step on the required Linux leg, so
the off path cannot rot. Added CI minutes are stated in the PR. Vendored C is compiled through ccache as the rest is.

## 10. Out of scope (follow-ups)

- Joints, including distance and revolute. The game needs none.
- Network: `b2World_GetStateHash` in the desync tree (small), and rollback through `b2World_GetSnapshot`, which upstream
  marks experimental with a format that will change.
- Editor gizmos, polygon vertex editing (a protocol minor), a Physics2D health row, Profiler stats.
- Capsule, segment and chain shapes; a 2D character mover (`b2World_CastMover`); render interpolation.
- Soft bodies, particles, GPU.

## 11. Risks

| Risk | Handling |
| --- | --- |
| Wrapper and bindings exceed the size headroom | Measured in the PR. A raise would be deliberate and recorded in `web/budgets.json`, as for Jolt |
| Determinism differs on MSVC or arm64, which were not measured here | The golden test fails CI. A difference is reported as a finding, and the golden is not loosened |
| Wasm SIMD off is slower for large 2D scenes | Not measured. Turning it on is one flag with an identical hash, but adds a browser requirement; a choice for later |
| Upstream API churn (v3.2 had breaking changes) | The pinned tag is byte-identical and Box2D types stay out of public headers, so an upgrade touches one module |
| `E:` is 95% full (27 GB free) | A worktree build is large. Check free space before starting |
| `Component` gains virtuals, a rebuild-everything header change | Conflicts with other open work are possible; #311 is the only open PR at the time of writing |

## 12. Stages

1. **`box2d-p1-vendor`:** starts now from master. Nothing links the library, so the dev size budget is not involved.
2. **`box2d-p2-module`:** starts after #311 merges, because of the dev `wasm.raw` budget in section 2.
3. **`box2d-p3-lua-docs`:** after stage 2.

Merging needs the user's go, as every code PR does.

## Deviations in stage 2

- `Physics2DWorld` is a thin abstract class with one implementation, not the concrete class of decision B. The reason is section
  13: the Box2D code stays in one file and a stripped build needs no stubs. It is still not a second-backend seam.
- Prefab overrides: the engine has no override system yet (EN4 is a proposal, `prefab-overrides.md`). Stage 2 tests that a
  prefab instance with a changed field simulates and keeps the field through a save and load, which is all that exists to test.
- `docs/physics2d.html` is created in stage 2, because the docs rule needs it in the same PR as the behaviour. Stage 3 adds Lua.

## 13. Stripping

The user asked (2026-11-08) that a game not using physics, or not using 2D physics, does not carry it. The wider feature
has its own proposal, `engine-stripping-design.md`. This module has to be strippable from the first line, so stage 2 has
these as acceptance criteria:

- **Compile time, because the linker cannot do it.** `RegisterBuiltinComponents` names every component type, so link-time
  dead-code elimination can never remove the module. With `N2ENGINE_USE_BOX2D=OFF`, all code that calls Box2D is compiled
  out and `box2d::box2d` is not linked. Everything that calls `b2*` lives in `engine/src/physics2d/`, behind that one switch.
- **Stubs stay.** The components still compile and register, as section 5 already planned, so scene data round-trips and
  `HashRegistry` is the same either way. They warn once and do nothing. A Lua `Physics2D` table is still present and its
  queries find nothing, so a script fails clearly rather than hitting a nil.
- **No cost when compiled in but unused.** The world is created by the first 2D component, so a 3D game pays one null check
  per step and no Box2D allocation.
- **Measured.** Stage 2 builds the size profile both ways and records the difference. Tests confirm that OFF builds and
  runs, and that a scene with 2D components loads and saves unchanged.

This changes nothing about the 3D path. Stripping the 3D backends is the larger saving and belongs to the wider proposal.
