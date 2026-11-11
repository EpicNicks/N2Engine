# Triangle-Like: Lua coverage audit (Phase 0)

Date: 2026-11-09. Base: `origin/master` at `66a49038` (after PR #315). Sprite2D is read from PR #318 (`feat-sprite2d-shapes`, not merged).

Plan: `E:/Code/Projects/Triangle-Like N2Engine/PLAN.md`, item 5 "Lua coverage". The plan keeps behaviour in Lua and asks for each missing binding to become a small C++ task.

## Summary

- Most of what Phase 1 to 3 need is bound today: lifecycle callbacks, time and time scale, action-map input (keyboard, mouse, gamepad), UI, prefabs, scene loading, layers, Physics2D (bodies, colliders, queries, 2D events), the camera, and Sprite2D once #318 merges.
- Three gaps hurt early:
  1. A script cannot reach another script's table. A projectile can find the enemy it hit, but it cannot call its `Damage`. This blocks Phase 1.
  2. There is only one seeded random stream (`Random.Seed`). The floor generator needs separate floor and reroll streams. This blocks Phase 2 (a pure-Lua generator is a workaround).
  3. There are no coroutines in Lua at all. `coroutine` is not opened, and the engine's C++ coroutines are not bound. Timers must be counted in `OnUpdate`. This does not block anything, but bosses (Phase 3) and dialogue (Phase 5) get much harder to write.
- Audio is partly bound. Lua can play, stop, loop and set the volume of an `AudioSource` set up in the editor. It cannot set the clip, play one-shots or set a mixer group's volume.
- The Phase 6 meta work has no bindings yet. There is no file IO or save data, no JSON from Lua, no raw key or text input for the debug console, and no touch input anywhere in the engine.
- The plan says "coroutines (std::generator with Unity-style waits)" are available. That is true for C++ only (`docs/coroutines.html`, section "Lua").

## Coverage table

"Yes" means verified in the binding source. "Partly" means part of the need is bound, or a workaround exists. "No" means nothing usable is bound.

| Capability | Needed for | Lua | Binding, or why not |
|---|---|---|---|
| Lifecycle callbacks | Everything | Yes | `LuaComponent.cpp` `CacheLifecycleMethods`: `OnAttach`, `OnUpdate`, `OnFixedUpdate`, `OnLateUpdate`, `OnEnable`, `OnDisable`, `OnDestroy`, `OnApplicationQuit`, `OnMouse*`. `self.gameObject` and `self.component` are set on the instance. |
| GameObject create, find, destroy, active, parent, tag, layer | Everything | Yes | `LuaCoreBindings.cpp` `GameObject` usertype: `Create`, `Destroy`, `SetActive`, `AddChild`, `FindChild(Recursive)`, `SetTag`, `CompareTag`, `SetLayer`, `AddComponent`, `GetComponent`. |
| Transform | Everything | Yes | `LuaCoreBindings.cpp` `Positionable`: `Get/SetPosition`, `Get/SetRotation`, `Get/SetScale`, `GetForward/Right/Up`. |
| Scene queries | Rooms, game context | Yes | `LuaCoreBindings.cpp` `Scene`: `FindGameObject`, `FindGameObjectsWithTag`, `GetRootGameObjects`, `DestroyGameObject`. `SceneManager.GetCurrentScene`. |
| Scene loading | Menus, hub, run start | Yes | `SceneManager.LoadScene(index or name)` (`LuaCoreBindings.cpp`). No additive load and no "keep across loads" object. Lua globals and `require` modules do survive a load, because there is one Lua state. |
| Calling another script | Damage, pickups, doors, room rules | **No** | The `LuaComponent` handle has only `SetScript`, `GetScriptPath`, `HasMissingScript` and the base `Component` methods (`LuaComponentBindings.cpp`). No call returns the script's instance table. A `GameObject` has no stable id to use as a table key: each push is a new userdata, and `__eq` does not apply to table keys. |
| Shared modules and globals | Game context, constants, data tables | Yes | `require` (`LuaRuntime.cpp` line 654). One shared state: a global from one script is seen by all (`docs/scripting-lua.html`, "LuaRuntime"). |
| Serializable fields and references | Tuning, prefab wiring | Yes | `SerializableFields`: numbers, bools, strings, Vector3, `GameObject` and component references, `Prefab` fields (`docs/scripting-lua.html`, "Reference fields", "Prefab fields"). Vector2 and Color defaults are not stored. |
| Prefab instantiate | Projectiles, enemies, pickups, rooms | Yes | `LuaPrefabBindings.cpp`: `Prefab.Instantiate(pathOrUuid, parent?, position?, rotation?)` and `handle:Instantiate(...)` on a `Prefab` field. `GameObject:GetPrefab()`. References out of the prefab come back nil, so the spawner wires them. |
| Coroutines and waits | Boss patterns, spawn waves, dialogue, hurt blink, door animations | **No** | Lua's `coroutine` library is not opened, and no scheduler or wait is bound (`docs/coroutines.html`, "Lua"; `LuaRuntime` opens only base, math, string, table). Workaround: timers and state machines in `OnUpdate`. |
| Time | Movement, cooldowns, pause | Yes | `LuaTimeBindings.cpp`: `deltaTime`, `time`, `fixedDeltaTime`, `unscaledDeltaTime`, `unscaledTime`, `tick`, settable `timeScale`. |
| Pause | Pause menu, time-stop item | Yes | `Time.timeScale = 0` stops fixed steps (`docs/scripting-lua.html`, lifecycle table). Menus can run on `unscaledDeltaTime`. |
| Keyboard | Movement, shooting, menus | Partly | Through action maps only: `Input.CreateActionMap` with `KeyboardButton` and `Vector2Composite` bindings, then `InputAction:GetVector2Value`, `WasPerformed` and `Subscribe` (`LuaInputBindings.cpp`). There is no `Input.GetKey(name)` and no typed-text input. |
| Mouse | Aiming, menus | Yes | `Input.GetMousePosition`, `GetMouseButton(Down/Up)`, `IsPointerOverUI`, `Mouse.Get():GetScrollDelta` (`LuaInputBindings.cpp`). |
| Gamepad | Twin-stick play | Yes | Action-map bindings `GamepadStick`, `GamepadAxis` and `GamepadButton` (`InputBinding.hpp`), made from Lua through `Input.CreateActionMap` (`ActionMapJsonFromLua`). The Lua path was read, not run, for gamepad types. |
| Touch | Touch UI (Phase 6) | **No** | The engine has no touch input (no hits for "touch" in `engine/src/input` or `engine/include/engine/input`). Whether the browser's touch reaches the web build as mouse input is not verified. |
| Screen to world | Mouse aiming | Partly | `Camera:ScreenPointToRay(x, y)` (`LuaCameraBindings.cpp`). For the orthographic camera, the ray's origin is the world point. There is no `ScreenToWorldPoint` helper. |
| Camera (orthographic, position) | Room camera snap | Yes | `Camera.Main()`: `SetPosition`, `GetPosition`, `SetOrthographic(left, right, bottom, top, near, far)`, `SetOrthographicResizeMode` (`LuaCameraBindings.cpp`). |
| UI: Canvas, Image, UIText, Button | HUD, menus, minimap, boss bar, shop | Yes | `LuaUIBindings.cpp`: `UI.CreateCanvas/CreateElement/CreateText/CreateButton`, `RectTransform` anchors and size, `Image:SetColor/SetSprite`, `UIText:SetText/SetFont/SetAlignment`, `Button:AddOnClick/SetInteractable`. There is no fill amount: a bar is done with `SetSizeDelta`. |
| World text | Damage numbers, prices | Yes | `TextRenderer` (`LuaComponentBindings.cpp`). |
| Audio: play, stop, loop, volume | SFX, music | Partly | `LuaAudioBindings.cpp`: `AudioSource:Play/Stop/Pause/IsPlaying/SetLoop/SetVolume/SetPitch/SetMixerGroup`, `Audio.SetMasterVolume`. `AudioSource::SetClip` and `SetPlayOnAwake` exist in C++ but are not bound. There is no one-shot. `AudioSystem::SetGroupVolume` is not bound. The clip must be set in the scene or prefab. |
| Music: intro then loop | Music manager | Partly | Two sources: play the intro without loop, then start the loop source when `IsPlaying()` turns false. The switch is at frame granularity, so there may be a small audible gap (not measured). No playback position and no queued clip. |
| Particles | Hit sparks, explosions | Yes | `ParticleSystem` (`LuaComponentBindings.cpp`). |
| Physics2D: bodies | Player, enemies, projectiles | Yes | `LuaPhysics2DBindings.cpp` `Rigidbody2D`: body type, mass, velocity, damping, gravity scale, fixed rotation, bullet, forces and impulses. `SetPosition` on a body teleports it (`docs/physics2d.html`). |
| Physics2D: colliders | Everything solid | Yes | `BoxCollider2D`, `CircleCollider2D`, `PolygonCollider2D`; base `SetIsTrigger`, `SetOffset`, `SetDensity`, `SetMaterial`, `SetSurfaceSpeed` (conveyors). |
| Physics2D: events | Hits, pickups, doors | Yes | `OnCollisionEnter2D/Stay2D/Exit2D` and `OnTriggerEnter2D/Stay2D/Exit2D` with `Collision2D` and `Trigger2D` (`LuaComponent.cpp` lines 1220 to 1225). |
| Physics2D: queries | Line of sight, AoE, turrets | Yes | `Physics2D.Raycast`, `RaycastAll`, `CircleCast`, `BoxCast`, `OverlapCircle`, `OverlapBox`, `SetGravity`. |
| Layers and collision matrix | Player, enemy, bullet layers | Yes | `Layers.SetName`, `GetMask`, `SetCollision` (`LuaCoreBindings.cpp`). The 2D world reads `Layers::` (`Physics2DWorld.cpp`). |
| Sprites and shapes | All game art | Yes after #318 | `Sprite2D` in #318's `LuaComponentBindings.cpp`: `SetShape` (Rect, Triangle, Circle, RegularPolygon, Polygon), `SetSize`, `SetSides`, `SetVertices`, `SetOutline`, `SetColor`, `SetTexture`, `SetRegion`, `SetFlipX/Y`, `SetPivot`, `SetSortingOrder`. There is no `SetMaterial` (a custom shader). |
| Shader effects | Glow, outline, rainbow, glitch, grayscale, time-stop | **No** | Sprite2D makes its own material (`EnsureMaterial`) and has no material setter. No full-screen pass exists in `engine/include`. This is engine work (plan item 3). |
| Seeded random | Floor generator, loot, item pools | Partly | `Random.Seed(n)` switches the one global `Random.*` to `Math::DeterministicRandom` (`LuaMathBindings.cpp`). That gives one stream, not separate floor and reroll streams. `math.random` is one global stream too. |
| Deterministic iteration | Floor generator, pools | Yes | `Ordered.pairs` (`LuaDeterministic.cpp`). `determinism.luaSeed` fixes `pairs` order and math functions. |
| Save data and file IO | Save, unlocks, settings | **No** | `io` and `os` are not opened. No binding writes or reads `user://`, even though the path exists in C++ (`ResourcePath.hpp`, `ResourceLoader.hpp`). |
| JSON from Lua | Save files | **No** | `LuaToJson` is C++ only (`LuaJson.cpp`). No `Json.Encode` or `Json.Decode` in Lua. Data tables can be Lua modules through `require`. |
| Settings | Settings menu | Partly | `Window.SetMode`, `Window.GetWidth/GetHeight`, `Audio.SetMasterVolume`. Music and SFX volumes need a group volume. Saving the settings needs save data. |
| Debug console | Debug console | Partly | `Debug.Log/Warn/Error` (`LuaDebugBindings.cpp`). No typed-text input, no raw key query and no log subscription to show logs in game. The console's commands can be plain Lua functions. |
| Platform query | Show touch UI on web and mobile | Partly | `Application.GetPlatform()` returns only Windows, macOS, Linux or "Unknown" (`LuaApplicationBindings.cpp`). The web build returns "Unknown". |
| Quit | Main menu | Yes | `Application.Quit()`. |

## Gaps, as small binding tasks (in priority order)

Sizes: XS is under 30 lines, S is under 150 lines, M is a few hundred lines. Each size includes tests, docs and the `engine-api.lua` annotation.

1. **Script instance access.** Blocks **Phase 1**. Size S.
   Add `LuaComponent:GetScript()`, which returns the instance table or nil, and `GameObject:GetScript(path)`, which finds the first `LuaComponent` running that script. Also add `GameObject:GetId()` with a stable integer, so tables can be keyed by object.
   Without it, a projectile cannot call the enemy's `Damage`. The only workaround is a global registry keyed by unique names, which the cleanup list wants to avoid.
   Check: the table must stop working when the component is destroyed, in the same way `ReleaseScript` nils `self.gameObject`.

2. **Seeded random streams.** Blocks **Phase 2**. Size S.
   Add `Random.NewStream(seed)`, which returns an object with `Float`, `Range`, `Int`, `InUnitCircle` and `GetState`/`SetState`, backed by its own `Math::DeterministicRandom`.
   Workaround: a pure-Lua PCG or xorshift on Lua's 64-bit integers.
   Note: neither option reproduces Unity's `System.Random`. The plan's "compare against the Unity generator's output" step then has to be a check of the rules, not of the exact rooms.

3. **Coroutines in Lua.** Slows **Phase 3** (bosses, waves) and **Phase 5** (dialogue). Size M.
   Option A: open Lua's `coroutine` library and add a small scheduler per `LuaComponent` (`self:StartCoroutine(fn)`, `Wait(seconds)`, `WaitUnscaled`, `WaitUntil(fn)`, `WaitForFixedUpdate`), resumed after `OnUpdate` and stopped on destroy.
   Check first: the instruction budget hook must also count instructions in coroutine threads (Lua copies the hook to new threads; this is not verified in the engine's sandbox). The panic and registry rules in `docs/scripting-lua.html` must also hold across a yield.
   Option B: bind the C++ `std::generator` coroutines. This is harder because a Lua function cannot be a C++ generator.
   Option A is the better fit.

4. **Audio clip and groups.** Partly blocks **Phase 1** (gun sounds overlap) and **Phase 6** (music, volume settings). Size S.
   Add `AudioSource:SetClip(path)`, `GetClip()`, `SetPlayOnAwake(bool)`, and `Audio.PlayOneShot(path, volume?, group?)` (a pooled source).
   Add `Audio.SetGroupVolume(group, v)`, `GetGroupVolume`, and `CreateMixerGroup`.
   Optional: `AudioSource:GetTime()`, or a "then play this clip" queue, for a gapless intro-to-loop switch.

5. **Save data and JSON.** Blocks **Phase 6** (save, unlocks, settings). Size S to M.
   Add `SaveData.Write(name, text)`, `SaveData.Read(name)`, `SaveData.Exists`, and `SaveData.Delete`, restricted to the project's `user://` folder, with plain file names only and a size cap.
   Add `Json.Encode(table)` and `Json.Decode(text)` over the existing `LuaToJson` and a JSON-to-Lua step.
   On the web, check where `user://` is stored (IDBFS or similar; not verified).
   Phase 4 unlock filtering can start with fixed defaults until this lands.

6. **Keyboard and text input for the debug console.** Blocks the **Phase 6** debug console. Size S.
   Add `Input.GetKey(name)`, `GetKeyDown`, `GetKeyUp` with the action-map key names, plus `Input.GetTypedText()` for this frame's characters, or a `OnTextInput` subscription from GLFW's char callback.
   Add `Debug.Subscribe(fn)` so the console can show log lines.

7. **Touch input.** Blocks **Phase 6** touch UI. Size M (engine C++, then the binding).
   Read touches in the web build and on desktop where GLFW allows it. Add `Input.GetTouchCount()` and `Input.GetTouch(i)` (id, position, phase), plus a virtual-stick helper, or feed touches to action maps.
   Also XS: make `Application.GetPlatform()` return "Web" under `__EMSCRIPTEN__`.

8. **Sprite effects.** Needed for the **Phase 6** effects pass (E3). Size M, mostly C++ rendering.
   Add `Sprite2D:SetMaterial(path)` after #318 merges. Add a full-screen pass with a Lua toggle (`Camera:SetPostEffect(name, params)` or similar) for grayscale and time-stop.

9. **Small helpers.** Not blocking. XS each.
   - `Camera:ScreenToWorldPoint(x, y)` for the orthographic camera.
   - `Image:SetFillAmount` for health bars.
   - Store Vector2 and Color defaults in `SerializableFields`. They are dropped today.

## How this was checked

- Read the binding files on `origin/master`: `engine/src/scripting/bindings/LuaAudioBindings.cpp`, `LuaTimeBindings.cpp`, `LuaInputBindings.cpp`, `LuaApplicationBindings.cpp`, `LuaDebugBindings.cpp`, `LuaUtilityBindings.cpp`, `LuaWindowBindings.cpp`, `LuaCoreBindings.cpp`, `LuaCameraBindings.cpp`, `LuaEventsBindings.cpp`, `LuaComponentBindings.cpp`, and the names in `LuaPhysics2DBindings.cpp`, `LuaUIBindings.cpp`, `LuaPrefabBindings.cpp` and `LuaMathBindings.cpp`.
- Read the lifecycle hook names in `engine/src/scripting/LuaComponent.cpp`.
- Read the `engine-api.lua` annotations and these sections of `docs/scripting-lua.html`: sandbox, determinism, fields, prefab fields, JSON, and "Where the file and the bindings differ".
- Read the "Lua" section of `docs/coroutines.html`.
- Read Sprite2D from #318's diff of `LuaComponentBindings.cpp` and `Sprite2D.hpp` in the read-only `sprite2d` worktree.
- Nothing was built or run. The gamepad path through `Input.CreateActionMap` and the browser's touch-to-mouse behaviour are not verified.
