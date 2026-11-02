# Proposal: prefab references in Lua (spawn a prefab from a script)

Status: design only, no code. Assumes **stable asset UUIDs** (stored in the .meta, carried by a move; decided with the asset-move work, PR #284). Protocol: this proposal takes **1.25.0** (the next free minor after MoveAsset's 1.24.0). `prefab-stage.md` takes 1.26.0; if the merge order flips, renumber, nothing else depends on it.

## 1. What exists today [V = verified in the tree]

- Prefabs are assets: `.prefab` JSON, `PrefabFile` loader, resource type `Prefab`, UUID in the .meta (`engine/include/engine/prefabs/PrefabFile.hpp`). Protocol 1.19.0 has `CreatePrefab`, `InstantiatePrefab`, `ApplyPrefab`, `UnpackPrefab` [V, roadmap section 10].
- `PrefabManager::InstantiatePrefab(json)` builds a tree with fresh UUIDs, remaps references inside the prefab, **nulls references to anything outside**, and puts the result in no scene [V, `PrefabManager.hpp`]. `InstantiatePrefabs` is the multi-root primitive.
- `GameObject::SetPrefabReference` / `GetPrefabReference` is an optional asset UUID, serialized as `prefabReference` [V, `GameObject.hpp:53,127`]. Create/Instantiate set it, Unpack clears it.
- Lua can instantiate a **model** only: `Model.Instantiate(path [, parent])` returns a `GameObjectRef`, owned by Lua until it is parented [V, `LuaComponentBindings.cpp` ~line 900-930]. There is no prefab equivalent.
- **Asset-typed Lua fields do not exist.** `MakeLuaFieldInfo` maps the declared `type` to: `GameObject` (GameObjectRef), a scriptable component name or `*Component` (ComponentRef), `string`, `int`/`integer`, `number`/`float`, `bool`/`boolean`, `Vector3`, else the default's kind [V, `engine/src/scripting/LuaFieldDeclaration.cpp:15-70`]. The docs table of reference types lists GameObject and components only [V, `docs/scripting-lua.html` "Reference fields"]. So a script that wants a prefab today can only hold a path string and has nothing to load it with.
- The only non-null `$ref` resolution is `LuaComponent.cpp` ~1470-1520, and it resolves through the scene's `ReferenceResolver` (objects and components), which cannot find assets.

## 2. The field: `prefab = { type = "Prefab" }`

**Declaration**

```lua
Spawner.SerializableFields = {
    enemy = { type = "Prefab" },
    count = 3,
}
```

**Storage in `scriptData`**: the asset UUID as a plain string, not a `$ref` object (a `$ref` is resolved against scene objects after load and an asset is not one):

```json
"scriptData": { "enemy": "f3c1...-uuid", "count": 3 }
```

Empty/absent means none. A string that is not a UUID is kept as written (so a hand edit is not lost), warns once at load, and reads as nil.

**Injection**: on `SetScript`/`Deserialize`, a `Prefab`-typed field injects a **prefab handle** (section 4) onto `self.enemy`, or nil. The handle is built lazily and holds only the UUID (no load at injection): loading happens at first `:Instantiate`, so a scene full of spawners does not load every prefab at scene load, and a missing prefab fails at use, with a clear message, not at load.

Engine changes:

- `FieldKind::PrefabRef` (new), `typeName = "Prefab"` in `MakeLuaFieldInfo`; `LuaDefaultKind` unaffected (no default beyond nil in v1; a path string default `res://prefabs/x.prefab` could be resolved to a UUID at extract time, but it would bake a path into the UUID store; leave out).
- `ExtractSerializableFields`: a `Prefab` field with no value stores nothing.
- `InjectFieldsIntoScript` and the reference-resolution loop: for `Prefab` fields, read the string, validate it parses as a UUID, build the handle. Never put it through `ReferenceResolver`.
- `SetComponentFields` (the editor write path): accept a UUID string or empty for a `PrefabRef` field and validate that it names an indexed asset of resource type `Prefab`; refuse (Error) any other asset type or an unknown UUID. This is the host-side enforcement of "only .prefab"; the UI is a convenience.
- `GetLuaFields` reports `kind: "PrefabRef"`, `typeName: "Prefab"`, and for the current value `assetPath` (res:// path, for display; the UUID stays the truth). That is the additive protocol change: **1.25.0**, Hello capability `prefabFields`.
- Because UUIDs are stable, a moved or renamed prefab keeps working with no rewrite of scenes; the asset-move work does not have to rewrite `scriptData` for this field.

## 3. Frontend part (frontend repo, outline only)

- The frontend already has asset-field machinery (texture/material/mesh fields). Add the `PrefabRef` kind to the field renderer: a slot showing the prefab's name and path, a picker listing only assets with type `Prefab` (`ListAssets` has no Prefab type in the roadmap's baseline; EN3 added it), a clear button, and drag-and-drop that accepts only `.prefab` rows from the Assets panel (reject others with the not-allowed cursor).
- Missing target: if the UUID is not in `ListAssets`, show the slot red with "Missing prefab <short uuid>" and keep the value until the user clears it (do not silently drop it).
- Double-click on the slot selects the asset in the Assets panel; with the stage feature (`prefab-stage.md`) it can open the stage.
- Writes go through `SetComponentFields`; multi-edit and undo behave as for other fields.
- Docs page and inspector tests: DOM tests with hand-made dataTransfer, as FE-S3 did.

## 4. Lua API

```lua
-- field handle
local go = self.enemy:Instantiate()                       -- at the origin, no parent, in the scene root
local go = self.enemy:Instantiate(parent)                 -- GameObject parent (adds as a child)
local go = self.enemy:Instantiate(parent, position)       -- Vector3, WORLD position
local go = self.enemy:Instantiate(parent, position, rotation) -- Quaternion, WORLD rotation

-- by path or UUID, mirroring Model.Instantiate
local go = Prefab.Instantiate("res://prefabs/enemy.prefab", parent, position, rotation)
local go = Prefab.Instantiate("f3c1...-uuid")
```

`parent`, `position`, `rotation` are all optional and positional; `nil` skips one (so `Instantiate(nil, pos)` spawns at a position in the scene root). Handle members: `handle.valid` (the asset exists and is a prefab), `handle.path`, `handle.uuid`, `handle.name`. Equality compares UUIDs.

Semantics:

- **Returns** a `GameObjectRef`. As `Model.Instantiate`: with a parent the parent owns it; without one, the new object is added as a **root object of the current scene** and the ref is non-owning. This differs from Model.Instantiate, which returns an owning ref that the script must parent or lose. Recommendation: for prefabs add to the scene root by default, because "spawn an enemy" that vanishes when the local goes out of scope is a trap; keep Model.Instantiate unchanged. State the difference in the docs.
- **Position/rotation** are applied after the tree is parented, as world values (matching the editor's `InstantiatePrefab` `position`, which is a world position). Absent: the prefab root's own saved local transform.
- **Active/attach**: components attach at the scene's next attach pass like any object added at runtime; `OnAttach`/`Start` of the instance run that frame, not inside the `Instantiate` call. The returned object can be configured immediately (set fields, move) before they run; document it.
- **External references**: nulled, exactly as `PrefabManager::InstantiatePrefab(json)`. A `GameObject`/component field in a script inside the prefab that pointed outside the prefab is nil after instantiation. This is by design (a prefab is a file and cannot know a scene's objects); the script should find its collaborators itself or the spawner should assign them on the returned object. Do **not** use the `outside` resolver overload for script spawns (that is for duplicate; here the "scene" is the running one and a stale UUID could bind a wrong object). Warn once per prefab (not per spawn) if the prefab data contains any reference that came back null, naming the prefab, so authors find out.
- **`prefabReference`**: the instance root gets `prefabReference = prefab UUID`, same as the editor's `InstantiatePrefab`, so a runtime-spawned instance carries its origin (useful for save games, debug, and for the network follow-up). It is not saved into the scene file unless the scene is saved from a play session, which the host forbids (play snapshots are not saved to scenes).
- **Fresh UUIDs** every call; instantiating twice never collides.
- `GameObject:GetPrefab()` (small addition, same PR): the handle for `prefabReference`, or nil.

## 5. Loading and caching (resource system)

- `Resources::LoadByUUID<PrefabFile>(uuid)` / by path through the existing `ResourceLoader` (the loader caches loaded assets by UUID; `PrefabFile` holds the parsed JSON, shared and read-only). `Prefab.Instantiate(pathOrUuid)` decides by shape: a string that parses as a UUID is a UUID, else a `res://` path (resolve path to UUID through the index; with stable UUIDs a path is just a lookup key).
- **Wrong type**: a UUID/path that resolves to a non-prefab asset: `error("Prefab.Instantiate: '<x>' is a Texture, not a Prefab")` (a Lua error, as `Model.Instantiate` throws `runtime_error` on failure). **Missing**: `error("Prefab.Instantiate: no prefab '<x>'")`. `handle:Instantiate()` on an invalid handle raises the same. These are catchable with `pcall`. A `Prefab`-typed field that is nil: `self.enemy:Instantiate()` is a nil index, the normal Lua error; document `if self.enemy then`.
- **Per-instantiate cost**: instantiate walks the JSON each time (as the editor does). v1 accepts that. If profiling says spawn-heavy scripts (bullets) suffer, a later optimization caches a parsed, validated form or a pre-built template tree; note it, do not build it.
- **Sanity limits**: the same depth/object caps as the editor path (`InstantiateEntities`: 10000 objects, 512 levels) apply to a script spawn, so a hostile or broken prefab cannot hang a frame; over the cap is a Lua error.
- **Hot reload**: a prefab asset changed on disk (ApplyPrefab, external edit) reloads through the resource system's normal path; existing instances are unchanged (v1 prefabs are copies with a link, no propagation), the next spawn sees the new data.

## 6. Threading

Game thread only. Lua runs on the game thread already; `Prefab.Instantiate` is bound next to `Model.Instantiate` and reads the resource cache, which `ResourceLoader` already guards for the loaders it has. Calling it from an editor-extension Lua state is **refused** (extensions are edit-mode scripts with the Editor.* API; spawning runtime objects there would bypass undo). Bind it only into the runtime Lua state, the same way `Model` is, and make sure the declaration evaluator state (which reads `SerializableFields` without running the script) does not need it: it only reads the `type` string.

## 7. Multiplayer

Facts [V, `docs/netcode.html` world section and `PrefabTable.cpp`, `NetworkWorld.cpp`]:

- Networked spawns come only from `NetworkServer::Spawn(prefabName, owner)`, which instantiates a **registered in-memory prefab** by name; the session freezes the registry into a table sorted by name (at most 1024), and a spawn message names the prefab by u16 index. Both ends build their table from their own registrations and the fingerprint's registry hash covers it. The doc itself says making prefabs files of the project is "left to the stage that adds them".
- A spawnable prefab must have exactly one `NetworkObject`, on its root. The prefab hash is of the prefab's shape (names, component type names, child counts), not UUIDs.
- A client can never spawn; there is no message for it.

Consequence for script spawns: `Prefab.Instantiate` creates a **local, non-networked** object. If the prefab root has a `NetworkObject`, a plain script spawn on a server would create an object that is not registered with the NetworkWorld: it would not replicate, its net id would not exist, and on a client it would be a divergent local copy. v1 must not let that happen silently:

- v1 behaviour: when a network session is active (server or client) and the instantiated tree contains a `NetworkObject`, `Prefab.Instantiate` raises a Lua error "prefab '<x>' has a NetworkObject: use Network.Spawn (not available yet)" and creates nothing. With no session (single player, edit-mode play), `NetworkObject` is inert and the spawn works. This is the safe default and costs one check.
- **Recommended follow-up (own PR, own design note, after netcode stages allow)**: replicate the **prefab UUID**. Concretely the netcode needs:
  1. Populate the engine's prefab registry from the project's `.prefab` assets at session start (N2Server and every client, the same set from disk), keyed by asset UUID (stable), instead of by the code-registered name. `PrefabTable::Capture` sorts by name; sort by UUID instead (or keep the name key and add the UUID), still capped at 1024.
  2. The fingerprint's registry hash must cover the table as before (shape hash per prefab) so a client with different prefab files refuses to join. Using the shape hash, not file bytes, keeps UUID-different but shape-equal edits compatible only if the table index matches; the UUID must therefore also be hashed (a different asset with the same shape is a different asset).
  3. A Lua `Network.Spawn(prefabHandleOrUuid, position?, rotation?, owner?)` server-only, wrapping `NetworkServer::Spawn`; on a client it errors (no client spawn, keep the guarantee). The spawn message either keeps the u16 table index (peers map index to the same asset because both tables are built from the same files and the hash agrees) or carries the 16-byte UUID. Recommendation: keep the index (smaller, no wire change) and rely on the table agreement the fingerprint already proves.
  4. `SpawnInstance` instantiates through the same `PrefabManager::InstantiatePrefab` path, so script, editor and network instances agree. Initial transform: add position/rotation to the Spawn message (today an object appears as its prefab made it).
  5. Late joiners, interest management and `prefabReference` on peers follow from the existing join-state machinery.
  6. Lockstep/rollback modes: spawns must be a deterministic event, not a server push; separate design. `Network.Spawn` is refused in a deterministic preset until that exists.
  Mark all of this **not part of the first PR**.
- Also noted: `Prefab.Instantiate` of a prefab without `NetworkObject` during a session is allowed and purely local (cosmetic effects); document that it is not replicated.

## 8. Export and web packaging

- **Web**: `web/CMakeLists.txt` packs `<project>/assets` (the whole folder, `GLOB_RECURSE assets/*`) and `project.n2proj` with `--preload-file`. So every `.prefab` under assets/ ships, **referenced or not**: a prefab used only from a Lua field is not missed. That is the good news.
- **The new problem, and it comes from stable UUIDs, not from this feature**: the .meta files live in `<project>/.import/...`, **outside `assets/`**, and are not packed. Today a UUID is derived from the path, so a shipped build with no .meta regenerates the same UUIDs. With stable UUIDs a build without the .meta would generate **new random UUIDs**, and every UUID reference in scenes, prefabs and `scriptData` would break at runtime (this affects existing scene references too, not only prefab fields). Required in the stable-UUID work, before or with this proposal: the packer must include `.import/` (a `--preload-file` of it, or better a generated `uuid -> path` index file in the pack), and the loader must read it when present. Flag this to the asset-move PR author; it is a gating risk for the web target.
- **Native export**: no export or packaging step exists in the repository (searched `scripts/`, `deploy/`, `cmake/`: only the web package). If an exporter that prunes unreferenced assets is added later, it must treat `Prefab`-typed fields in `scriptData` (and `prefabReference`) as dependency edges, and string paths passed to `Prefab.Instantiate` are invisible to it (document: use a field, or keep the prefab in a folder the exporter always includes). This is the reason to prefer the field over a path string, and the docs should say so.
- The web build links the software renderer and single-thread Lua; `Prefab.Instantiate` has no platform-specific code.

## 9. Errors and warnings (summary)

| Case | Result |
|---|---|
| Field value not a valid UUID | Warn at load, reads nil, value kept |
| Field UUID not an indexed asset | Handle with `valid=false`; error at `:Instantiate` |
| Asset is not a `Prefab` | Lua error naming the real type (also refused by `SetComponentFields`) |
| Prefab JSON malformed / over caps | Lua error; nothing added to the scene (all or nothing, `InstantiatePrefab` returns null and logs) |
| Prefab has `NetworkObject` and a session is active | Lua error (v1), see section 7 |
| Instantiate in an extension Lua state | Not bound: `Prefab` is nil there |
| External references nulled | One warning per prefab per process |
| Called off the game thread | Not possible from Lua; C++ callers must not |

## 10. Test plan

- C++ (`engine_tests`): field declaration reports `PrefabRef`; extract/inject round trip through `scriptData` (UUID string, empty, invalid string kept); `SetComponentFields` refuses a texture UUID, an unknown UUID, accepts a prefab UUID; handle instantiation returns a parented object, a scene-root object, applies world position/rotation, sets `prefabReference`, gives fresh UUIDs on two calls, nulls an outside reference and warns once; wrong type/missing/malformed errors catchable with `pcall`; caps; `Prefab` absent in the extension state; a prefab spawn with a `NetworkObject` under a simulated session errors and adds nothing.
- Stable-UUID dependent: move a prefab (asset-move test helper) and spawn through the field afterwards.
- Protocol: `GetLuaFields` golden for the new kind, protocol.json and test-vectors bump to 1.25.0, Hello capability, mcp/protocol docs checks.
- Lua docs: add the field-type row, the `Prefab` API section and a worked spawner example; keep `docs/scripting-lua.html`, `docs/editor.html` and `engine-api.lua` annotations in the same PR (project rule).
- Frontend: DOM tests for the picker, drag accept/reject, missing state, `SetComponentFields` payload.
- Manual: spawner script in `lua_project`, instantiate 1000 in a loop to read the cost; web build smoke (`web-package-smoke`) with a prefab spawned from a field once `.import` is packed.

## 11. Size, risk, PR order

Size: engine **M** (field kind, handle, `Prefab` table, `GetPrefab`, checks, docs), protocol additive **S**, frontend **M**. Network follow-up **L** and separate.

Risks: (1) the `.import` packaging gap (section 8) is real and independent of this feature; (2) the ownership difference from `Model.Instantiate` can confuse, so it must be documented and tested; (3) scripts spawning hot-loop objects are slow until a template cache exists; (4) users will expect spawned `NetworkObject` prefabs to replicate: the v1 error is deliberate.

PR order:
1. Engine + protocol 1.25.0: `PrefabRef` field kind, injection, `SetComponentFields` validation, `Prefab` Lua API, `GameObject:GetPrefab`, docs, tests. (Needs stable UUIDs merged first, or runs on path-derived UUIDs with the same code, since only the lookup differs.)
2. Packaging: `.import` (or uuid index) in the web pack, with the stable-UUID work; could precede 1.
3. Frontend: `PrefabRef` field UI and protocol sync.
4. Docs-only ledger/MCP entries.
5. Later: network replication of prefab UUIDs and `Network.Spawn`.

## DECISIONS for the user

1. **Scene-root default for `Prefab.Instantiate` without a parent** (non-owning, differs from Model.Instantiate). Recommendation: yes, add to the scene root.
2. **Script spawn of a `NetworkObject` prefab during a session**: error in v1, replicate in a follow-up. Recommendation: error now, follow-up PR replicating by UUID-keyed prefab table.
3. **External references**: nulled (no resolver) for script spawns. Recommendation: nulled, with one warning per prefab.
4. **Prefab field default**: none (nil only), no path-string default. Recommendation: none in v1.
5. **Packaging of `.import`**: make "ship the .meta (or a uuid index) in every package" a requirement of the stable-UUID work. Recommendation: yes, gate stable UUIDs on it for the web target; generated index file preferred over shipping `.import`.
6. **Extensions**: `Prefab` not available to editor extensions. Recommendation: not available.
7. **Protocol number**: this proposal 1.25.0, the stage 1.26.0. Recommendation: confirm, renumber if the merge order changes.
