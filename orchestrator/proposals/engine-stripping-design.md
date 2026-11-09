# Engine stripping: design

Item: `orchestrator/items/engine-stripping.json` (gated). Requested by the user on 2026-11-08: "make engine stripping when
engines are not used, for example if a game doesn't use physics or doesn't use 2D physics in particular".

**Status: decided by the user on 2026-11-08 (section 5). Stage 1 is in progress (`engine-stripping-p1-physics`).**

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

1. **Modules.** All of them, in the order of section 7. The physics group comes first because it proves the mechanism and
   is the largest known saving: `physics3d` (none, physx, jolt or both) and `physics2d` (on or off).
2. **Choosing.** The project lists its modules. A scan tool reports what the project uses and checks the list against it.
3. **The scan** reads scenes and prefabs for component type names, and Lua scripts for `Physics2D.`, `Physics.`,
   `Raycast` and component names. It can only be a helper for Lua, because `AddComponent(name)` with a computed string
   cannot be proven statically. So the scan can suggest but not guarantee.
4. **Safety.** A scene that names a stripped component fails at package time with a message naming the scene and the
   module. If something still gets through at run time (a computed name in Lua), the component logs one clear error
   and does nothing, rather than crashing or silently losing data.
5. **Stubs.** A stripped module's component types stay registered as thin data holders. Scene data survives a round trip,
   the editor still lists them, and `HashRegistry` (the network fingerprint, which hashes type names and replicated fields)
   does not change. A stub must be small enough for its cost to be negligible; that gets measured.
6. **Where it applies.** A game's shipped build, on the web and natively. The editor and host always build everything,
   since a project can use anything while being edited. `engine` is one static library, so a stripped game needs its own
   build tree with its own options; the module list is what sets them.
   - **Web:** `N2WebPlayer` built with the project's list.
   - **Native:** there is no generic native player. A game is its own executable linking `engine`, like `lua_project` and
     `test_project`, and has no package step. So native gets a CMake helper that reads the project's list and sets the
     options, and the scan runs as a command the game's build can call. Whether a generic native player should exist is
     a separate question and is not part of this.
7. **CI.** Each stripped configuration is built and smoke-tested, or it will rot. Every module on every platform is too many
   builds, so CI builds one configuration per module with only that module off, plus one minimal configuration with all
   of them off, on the web and on one native Linux leg. That adds build minutes, stated per stage.

**How Box2D fits:** Physics2D stage 2 is built this way from the start. With the option off it leaves no Box2D code in the
binary, and with it on, a project with no 2D components creates no world and pays only a null check per step. Section 13 of
`physics2d-box2d-design.md` makes that an acceptance criterion, so this work does not have to retrofit it.

## 5. Decisions (user, 2026-11-08)

| | Decision | Answer |
| --- | --- | --- |
| 1 | Is the list explicit, automatic, or both? | **Explicit list, with the scan suggesting and checking.** Fully automatic could strip something a Lua script reaches through a computed name, and the game then breaks only when that line runs |
| 2 | Which platforms? | **Both web and native** |
| 3 | Which modules? | **All of them**, not only physics |
| 4 | When? | **After Physics2D stage 2**, so 2D is in the module list from the start |

## 6. Not verified

- The size of a real no-physics player. The table in section 1 is history; the first task is to build one and measure it.
- Whether both 3D options off currently builds on the web. Existing CI legs have at least one backend on, so expect fixes.
- How much of `CharacterController`, `SoftBody`, particles and `RollbackWorld` depends on a 3D backend being compiled in.
  They accept a null backend at run time, but compile-time removal may need more.
- What any module other than physics saves. None has been measured.
- Whether each module can be removed cleanly. Section 7's coupling notes come from a quick look at the sources, not a build.

## 7. Stages

Each stage is its own PR and needs the user's go to merge. Each one records the measured size of its configurations.

| Stage | Modules | Size of source (lines in `engine/src`) | What makes it hard |
| --- | --- | --- | --- |
| 1 | The mechanism: module list, scan tool, CMake helper, package-time check. `physics3d`, `physics2d` (this is J10) | | Both backends off on the web has probably never been built |
| 2 | `particles`, `softbody` | 838, 990 | Both have physics-backed paths, so they depend on stage 1 |
| 3 | `audio`, `ui` | 1,565, 1,517 | `N2ENGINE_WEB_AUDIO=stub` shows audio can already be stubbed; `Application` initialises audio directly |
| 4 | `replay`, `network` | 2,046, 14,528 | The largest module. Replay and rollback depend on it, and the network fingerprint is defined by the registry, so a stripped game cannot be a peer of a differently built one unless the stubs hold |
| 5 | `scripting` | 6,224 | Last, because every module above has Lua bindings. A no-Lua game also drops sol2 and Lua. Component names reach Lua through the bindings, which need splitting per module |
| 6 | Renderer backends and font and model importers | not counted | Native already has options for some (`N2ENGINE_RENDERER_VULKAN`, FreeType, ufbx). This stage puts them on the same list |

Not strippable: the core (`GameObject`, `Scene`, `Component`, transforms, serialization, `Application`), and one renderer.

**The scan gets weaker for later modules.** Component names in scenes are easy to find, but audio, UI and networking are often
reached from Lua by name. That is why the check at package time and the clear error at run time matter more than the scan.

**After stage 1, stop and compare.** Stage 1's real numbers and the first module's cost show whether later stages are worth
what they take. The user asked for all modules, so the plan keeps them; the numbers get reported at each stage so a stage can
be dropped knowingly if it saves little.
