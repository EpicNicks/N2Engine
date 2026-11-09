# Engine stripping: design

Item: `orchestrator/items/engine-stripping.json` (gated). Requested by the user on 2026-11-08: "make engine stripping when
engines are not used, for example if a game doesn't use physics or doesn't use 2D physics in particular".

**Status: proposal, waiting for the decisions in section 5.** No code has changed for this.

## 1. Why it is worth doing

Every game's web package is the same player, carrying both 3D physics backends. The size profile's wasm on master is
1,901,099 bytes brotli. The PRs that linked the backends recorded what each added at the time (`web/budgets.json`):

| Module | Added, wasm raw | Added, wasm brotli | Share of today's brotli |
| --- | --- | --- | --- |
| PhysX (PR #184) | 2,740,965 | 771,663 | 41% |
| Jolt (PR #205) | 978,104 | 272,325 | 14% |
| Box2D, library only (probe, 2026-11-08) | 168,629 | 63,853 | 3% |

These are historic deltas that have not been re-measured, and shared code means they will not add up exactly. Even so, a
2D game could plausibly lose around half its wasm by dropping the 3D backends. That is far more than Box2D itself costs,
so **the 3D backends are where the benefit is, and 2D is the smaller part.**

## 2. What exists today

- Compile-time options: `N2ENGINE_USE_PHYSX`, `N2ENGINE_USE_JOLT`, `N2ENGINE_USE_BOX2D` (stage 1 of Physics2D),
  `N2ENGINE_RENDERER_VULKAN`, `N2ENGINE_RENDERER_WEBGPU`, `N2ENGINE_WEB_AUDIO=stub`.
- The engine already copes with having no 3D backend: `Get3DPhysicsBackend()` is null and components accept that.
- J10 in the physics plan ("web packages link only the selected backend") was planned and never done. This item covers it.
- There is no per-game build. `N2WebPlayer` is one target, and the project's data is packed separately.

## 3. Why the linker cannot do it

`RegisterBuiltinComponents` names every component type, which links each one and everything it reaches, and
`Application.cpp` refers to the backends directly. So LTO and dead-code elimination cannot remove a module, however unused.
Stripping has to happen at compile time: a stripped module's code must not be referenced at all.

## 4. Proposed approach

**A per-project module list, used when building that game's player.**

1. **Modules.** Start with the physics group: `physics3d` (none, physx, jolt or both) and `physics2d` (on or off). Others
   (audio, networking, Lua, UI) can follow once measured, and only if worthwhile.
2. **Choosing.** The project lists its modules. The package step scans the project and reports what it found.
3. **The scan** reads scenes and prefabs for component type names, and Lua scripts for `Physics2D.`, `Physics.`,
   `Raycast` and component names. It can only be a helper for Lua, because `AddComponent(name)` with a computed string
   cannot be proven statically. So the scan can suggest but not guarantee.
4. **Safety.** A scene that names a stripped component fails at package time with a message naming the scene and the
   module. If something still gets through at run time (a computed name in Lua), the component logs one clear error
   and does nothing, rather than crashing or silently losing data.
5. **Stubs.** A stripped module's component types stay registered as thin data holders. Scene data survives a round trip,
   the editor still lists them, and `HashRegistry` (the network fingerprint, which hashes type names and replicated fields)
   does not change. A stub must be small enough for its cost to be negligible; that gets measured.
6. **Where it applies.** The editor and host always build everything, since a project can use anything while being edited.
   Stripping applies to a game's shipped player.
7. **CI.** Each stripped configuration is built and smoke-tested, or it will rot. That is web-job minutes; the first
   version should add a single "no physics" configuration and report its size.

**How Box2D fits:** Physics2D stage 2 is built this way from the start. With the option off it leaves no Box2D code in the
binary, and with it on, a project with no 2D components creates no world and pays only a null check per step. Section 13 of
`physics2d-box2d-design.md` makes that an acceptance criterion, so this work does not have to retrofit it.

## 5. Decisions needed

| | Decision | Recommendation |
| --- | --- | --- |
| 1 | Is the list explicit, automatic, or both? | **Explicit list, with the scan suggesting and checking.** Fully automatic could strip something a Lua script reaches through a computed name, and the game then breaks only when that line runs |
| 2 | Which platforms? | **Web first.** Download size is where it matters most, and web packages already build per profile. Native can reuse the same list later |
| 3 | Which modules first? | **Physics only** (3D backends and 2D), as asked. Others need measuring to show they are worth it |
| 4 | When? | **After Physics2D stage 2**, so 2D is in the module list from the start. The 3D part could start sooner if size matters now |

## 6. Not verified

- The size of a real no-physics player. The table above is history; the first task is to build one and measure it.
- Whether both 3D options off currently builds on the web. Existing CI legs have at least one backend on, so expect fixes.
- How much of `CharacterController`, `SoftBody`, particles and `RollbackWorld` depends on a 3D backend being compiled in.
  They accept a null backend at run time, but compile-time removal may need more.
