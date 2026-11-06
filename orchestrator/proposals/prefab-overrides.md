# Proposal: prefab instance overrides (EN4)

Status: design only, no code. Written against master at 448b06e5 (editor protocol **1.26.0**: prefab assets 1.19.0, prefab references 1.25.0, prefab stage 1.26.0 all merged). Tags: [V] verified in the tree at that commit, [M] from memory or general knowledge, not checked here, [E] estimate. Protocol: this proposal takes the next free minor; the working number is **1.28.0**, after EN5 host stats (`engine-stats-profiler.md`, 1.27.0). Renumber if the merge order flips; nothing else depends on the numbers.

EN4 in the roadmap (`electron-ui-roadmap.md` section 10 and the table, row EN4, "Prefab overrides v2 (source ids, diff, revert, indicator data, propagation)", L+, needs its own design doc). FE-S4 (overrides indicator and revert UI) follows it. CONTINUE.md queue item 5 lists "EN4 overrides (needs its own design doc first)". This is that doc.

## 1. Goals

1. An instance knows which of its objects and components came from which object and component of the prefab (a **correspondence** that survives save, load, copy/paste and Apply).
2. The host can say, per instance, **what differs from the prefab** (field-level overrides, added and removed objects and components) for an indicator (bold fields, a dot on hierarchy rows).
3. **Revert** an override (one field, one component, one object, or the whole instance) and **apply** selected overrides to the prefab.
4. **Propagation**: a change to the prefab file reaches its instances, keeping each instance's overrides.
5. Old scenes and prefabs keep loading; nothing changes for a game at runtime.

Out of scope: nested prefabs and variants (v3 in the roadmap); a project-wide rewrite of scene files that are not open (decision 3 covers what happens to them).

## 2. What exists today [V]

- **Instances are copies with a link.** `GameObject::_prefabReference` (`engine/include/engine/GameObject.hpp:53`, accessors 127-128) is an optional asset UUID, serialized as `prefabReference` on the object (`engine/src/gameobject/GameObject.cpp:944-947`, read back at 1041-1044). Only the instance root carries it.
- **Instantiate renumbers everything.** `PrefabManager::InstantiatePrefabs` (`engine/src/prefabs/PrefabManager.cpp:213-276`) gives every GameObject and component id a fresh random UUID (`CollectIds`, 53-101), then `Renumber` (110-153) rewrites **every string in the data that equals one of the old ids** to the new id, and nulls a Lua `$ref` that points outside. The old-to-new map is local to `InstantiateMany` and thrown away. So after `InstantiatePrefab` nothing records which instance object came from which prefab object: this is the gap the roadmap names.
- **The prefab file's ids are the ids of whatever object it was last written from.**
  - `CreatePrefab` writes `PrefabJsonOf(entity)` (`editor-server/src/EditorServer.cpp:9012-9027`, used at 9090): the source object's own UUIDs, root `prefabReference` erased. The source object then gets the link (9112-9120). So right after Create, the source instance and the file share UUIDs.
  - `ApplyPrefab` writes `PrefabJsonOf(entity, prefabName, rootName)` (9300) from the **instance's** UUIDs, keeping only the prefab's and root's names. Every Apply from a different instance therefore **renumbers the whole file**.
  - `SavePrefabStage` writes the stage root, whose objects were built with the **file's own UUIDs** (`HandleOpenPrefabStage`, 9714-9736: `GameObject::Deserialize`, not `InstantiatePrefab`), so a stage save keeps the file's ids (9896-9908).
- **`InstantiatePrefab`** (9127-9238): reads the file from disk (`ReadPrefabFile`, 8984-9008), checks caps (`MaxClipboardObjects` 10000, `MaxClipboardValues` 4,000,000, `MaxClipboardBytes` 16 MiB: 542-544), calls `InstantiatePrefabs({&rootJson}, nullptr)` (outside references null), sets the root's `prefabReference`, records one undo step `CreatedOp`.
- **`UnpackPrefab`** (9320-9356) clears the root's link only, one undo step `PrefabLinkOp`.
- **Lua `Prefab.Instantiate`** (1.25.0) uses the same primitive and sets the link (`engine/src/scripting/bindings/LuaPrefabBindings.cpp:231-237`).
- **Copy/paste and duplicate** go through the same renumbering (`InstantiatePrefabs` at 7123 for paste, `InstantiatePrefab(json, sceneReferences)` at 6829 for duplicate), so a pasted instance root keeps `prefabReference` (it is ordinary data) and gets fresh ids.
- **Serialized shape** of an object: `uuid, name, tag, layer, isActive, prefabReference?, positionable {localPosition, localRotation, localScale}, components [{type, data}], children []` (`GameObject.cpp:932-968`, `engine/src/Positionable.cpp:339-349`). A component's id is `data.uuid`. Deserialize ignores keys it does not know (`GameObject.cpp:1001-1086` reads named keys only), so **additive keys are backward compatible on load** (an older engine drops them on its next save).
- **Field addressing for edits**: `SetComponentFields` names a component's top-level keys, and a container field (a LuaComponent's `scriptData`) by its inner keys as `scriptData.speed` (`EditorServer.cpp:7407-7434`); `DescribeFields()` reports each field's `container`. The same leaf paths are the natural override paths.
- **Undo**: big changes record `SnapshotScene` / `RestoreSceneSnapshot` (`EditorServer.cpp:10083`, `10107`; used at 5791-5823 and 7347-7370).
- **Asset changes** all arrive in `ApplyAssetChanges` (after `NoteAssetWritten`, 7859-7880, for the host's own writes, and after `PollAssets`, 7882-, for outside edits).
- **Wire**: `HierarchyNode` and `EntityHeader` have `prefab?: string` (`editor-server/protocol/protocol.json:1294-1316`); Hello capabilities are a fixed list (`EditorServer.cpp:2886-2890`: ... `prefabAssets`, `prefabFields`, `prefabStage`, `prefabStageContext`).
- **Nested links**: the stage treats objects under the root that have their own `prefabReference` as plain objects and counts them as a warning (9738-9745); `SavePrefabStage` refuses a self-link (`prefabCycle`, 9868-9885).

## 3. The correspondence: `prefabSource`

Every object and component of an instance carries the id of the prefab-file object or component it corresponds to.

- GameObject JSON gains `"prefabSource": "<uuid>"` next to `prefabReference`.
- A component entry gains a sibling key, `{"type", "data", "prefabSource"}`, so no component's own `Serialize` changes.
- In memory: `GameObject::_prefabSource` and `Component::_prefabSource` (`std::optional<Math::UUID>`), not exposed to Lua.

Rules:

| Path | `prefabSource` |
|---|---|
| `CreatePrefab` | every object and component of the source subtree gets its **own** UUID as source (the file was written with those ids). |
| `InstantiatePrefab`, `Prefab.Instantiate` (Lua) | the **old** id from `InstantiateMany`'s id map (new id -> file id). `InstantiatePrefabs` gets an optional out-parameter returning the map; the caller sets the sources after building. |
| Copy/paste, duplicate of a whole instance | kept as they are (they name file ids, not scene ids). |
| Paste of part of an instance (no linked root above it) | dropped by a **normalisation** pass: a `prefabSource` is meaningful only under a root with `prefabReference`. The same pass runs on scene load. |
| `UnpackPrefab` | clears the root link **and** every `prefabSource` in the subtree, in the same undo step. |
| Added object or component in an instance | none (that is what makes it "added"). |

**`Renumber` must skip the `prefabSource` key.** It rewrites any UUID string equal to an id in the map (`PrefabManager.cpp:117-126`). After `CreatePrefab` the source ids **equal** the instance's own ids, so a duplicate of that instance would have its sources rewritten to the copy's fresh ids and lose the correspondence. Skipping the key (as the code already treats `$ref` specially) fixes it; a unit test pins it.

**`ApplyPrefab` must write the file's ids, not the instance's.** It serializes the instance and then maps every id through `prefabSource` (instance id -> file id), so the file keeps its UUIDs. An added object or component gets a fresh file id, and the instance's new `prefabSource` is set to it in the same step. Without this every Apply renumbers the file and breaks every other instance's correspondence. The stage already preserves file ids (section 2), so after this change all three writers (Create, Apply, stage Save) agree.

## 4. What an override is

The **override set** of an instance is the difference between the instance and the prefab, in prefab terms. Leaves:

- object header: `name`, `tag`, `layer`, `isActive`;
- `positionable.localPosition`, `.localRotation`, `.localScale` (each one leaf);
- each top-level key of a component's `data` except `uuid`; for a container field (`DescribeFields().container`, today `scriptData`) each inner key (`scriptData.speed`), matching the `SetComponentFields` paths;
- arrays (AssetRefList, GameObjectRefList and the like) are one leaf each (no element-level merge in v1).

Structural:

- **added object**: an object under the instance with no `prefabSource`; **added component**: likewise.
- **removed object** / **removed component**: a file object or component whose id no instance object or component names as its source.
- Child order: v1 follows the prefab's order for corresponding children and puts added children after them in their own order; a reorder is not an override.

**Instance-owned values, never overrides** (Unity's rule [M]): the root's `name`, `positionable.localPosition` and `localRotation`, and the root's parent and sibling index. `InstantiatePrefab` already sets the root position (9222-9230) and `ApplyPrefab` keeps the file's root name (9288-9299). The root's scale and everything else are ordinary leaves.

**Comparison.** Values are compared as JSON after mapping references: a UUID string equal to an id of an object or component **inside the instance** is replaced by that object's `prefabSource` before comparing, so an internal reference that points at the corresponding object is not an override. A reference to anything outside the instance is always an override, because the file has null there (`InstantiatePrefab` nulls outside references). Asset UUIDs are left alone, as `Renumber` already does (`PrefabManager.cpp:103-107`). Floats compare exactly; a value nudged back to within a rounding error stays an override until reverted (documented).

## 5. How overrides are found and stored (decisions 1 and 2)

| Option | How | Verdict |
|---|---|---|
| A. Diff on demand only | compare instance and current prefab whenever asked; store nothing | **Rejected**: after the prefab changes, a value that differs could be an override or a stale old prefab value; propagation cannot tell which. |
| B. Record overrides on every edit path (Unity's PropertyModifications [M]) | every Write command that changes an instance adds the path to a list | Works, but every edit path must remember: SetComponentFields, SetEntityProperties, SetLocalTransform, gizmos, AddComponent/RemoveComponent, reparent, paste, undo/redo, Lua extension `Editor.Scene` writes. A missed path loses the user's change at the next propagation, silently. |
| **C. Diff against the prefab the host keeps in step, store the result** | while a scene is open the host keeps every instance **reconciled** with the current prefab (section 6), so `diff(instance, prefab)` **is** the override set at any moment; the scene file stores the set's **paths** (not values: the values are in the instance data) | **Recommended.** No edit path changes; any edit, undo or extension write is seen. The stored paths are what a later load needs to know which values were the user's. |

Stored form (option C), on the instance root only:

```json
"prefabReference": "f3c1...",
"prefabOverrides": {
  "v": 1,
  "modified": [ {"source": "<file object id>", "component": "<file component id>?", "path": "scriptData.speed"} ],
  "removedObjects": ["<file object id>"],
  "removedComponents": ["<file component id>"]
}
```

Added objects and components need no entry (no `prefabSource`). The list is recomputed from the diff when the scene is serialized (save, autosave, play snapshot), so it is never edited by hand-written code.

The host keeps, per prefab UUID, the **base**: the prefab JSON the open scene's instances were last reconciled with (in memory, `EditorServer`). Cost: one parsed copy per prefab used in the open scene (each capped at 16 MiB as text by the existing limits).

## 6. Reconcile (propagation) (decision 3)

`Reconcile(instance, base, newPrefab)`:

1. overrides = `diff(instance, base)` (or, at load, the stored `prefabOverrides`);
2. for every prefab leaf **not** overridden: write the new prefab's value into the corresponding instance object or component (in place: header setters, `Positionable` setters, `component->Deserialize(merged data)` as `SetComponentValues` does, then `OnEditorFieldsChanged`);
3. objects and components new in the prefab: created in the instance with fresh scene ids and `prefabSource`; references inside them mapped file id -> instance id;
4. objects and components gone from the prefab: removed from the instance, **unless** the instance overrode something on them (then they are kept as added objects, with a warning, so no user value is dropped silently);
5. overrides on objects the prefab removed and `removedObjects` naming ids the prefab no longer has are dropped.

In place, not by rebuilding the subtree: a rebuild would leave references from outside the instance pointing at destroyed objects (`RestoreSubtree` re-resolves through the scene only for its own ids, 1534-1564, and the destroy path clears referrers via `ForgetDestroyed`, 1523-1527).

**When it runs** (recommended):

- **Scene open** (`OpenSceneFile`, `RestoreAutosave`, the stage's Close restoring the parked scene): every linked instance is reconciled with its prefab file using the stored `prefabOverrides`. Not an undo step (the scene is being loaded); the scene is marked dirty only if something actually changed, with a log line "Updated N instances of M prefabs".
- **A prefab file changes while a scene is open** (`ApplyAssetChanges` with a `.prefab` in `modified`: the host's own `ApplyPrefab`, `SavePrefabStage`, or an outside edit picked up by `PollAssets`): its instances in the open scene are reconciled from the base to the new file as **one undo step** "Update instances of X", recorded with `SnapshotScene`/`RestoreSceneSnapshot`, and the base moves on. An undo then shows the old values as overrides, which is truthful; redo re-applies. While a prefab stage is open the scene is parked: its instances reconcile at Close.
- **Never at game runtime.** Scene files stay self-contained (full instance data), so `Scene::FromJSON`, the play host, `n2server` and the web player load exactly what was saved, and determinism hashes do not depend on prefab files.

Scenes that are not open keep the old prefab content until they are next opened in the editor. A packaged build (web: `web/CMakeLists.txt` packs `assets/` as files [V per prefab-references.md section 8]) would ship that stale content. Decision 3 asks whether to add a host command that reconciles and saves every scene of the project after a prefab change; the recommendation is a later, explicit "Update prefab instances in all scenes" command, not an automatic rewrite.

## 7. Legacy instances (decision 4)

Scenes saved by 1.19.0-1.27.x have instances with `prefabReference` and no `prefabSource`. At first open:

- **Shape match**: if the instance tree has exactly the prefab's shape (the same child counts at every level, the same component type sequence per object), assign `prefabSource` by position. Then take a diff against the **current** file as the override set (the best available guess: values that differ become overrides, so nothing the user sees changes on open). The scene becomes dirty; a log line names the instances.
- **No match**: the instance stays a **legacy link** (`prefab` set, `overridesAvailable: false` on the wire); it is never reconciled; Apply and Unpack work as today; the inspector offers "Replace with a fresh instance" later (frontend). No guessing by names.

## 8. Commands and wire (protocol 1.28.0, working number)

Hello capability `prefabOverrides`. All additive; ids chosen at implementation from the free ones (commands free now include `0x2F`, `0x42`-`0x4F`, `0x54`-`0x5F`; responses `0x27` onwards, if EN5 does not take them first [V `protocol.json` id lists]).

| Command | Class | Meaning |
|---|---|---|
| `GetPrefabOverrides {entityId}` | Read | `entityId` is any object of an instance; answers `PrefabOverrides` for its instance root: `{root, prefabUuid, path, legacy, modified: [{entityId, componentId?, path, value, prefabValue}], addedObjects: [entityId], addedComponents: [{entityId, componentId}], removedObjects: [{sourceId, name, parentEntityId}], removedComponents: [{entityId, sourceId, type}]}`. Values are JSON as `GetEntity`/`GetComponent` show them. Error for an object not under a linked root. Capped like `SerializeEntities` (the answer is at most the instance's size). |
| `RevertPrefabOverrides {entityId, targets}` | Write | `targets` (JSON): `[{entityId, componentId?, path?}]`, or `[{all: true}]` for the whole instance. A path reverts one leaf; a component without a path reverts all its leaves; an object without component reverts its header, transform and components; `{entityId, removedSourceId}` restores a removed object or component; an added object or component as target removes it. One undo step "Revert X". |
| `ApplyPrefabOverrides {entityId, targets}` | Write | Same targets; writes those overrides into the prefab file (file ids kept, section 3), then reconciles every instance in the open scene (section 6). The file write is not undoable (as `ApplyPrefab`); the scene part is one undo step. Refused by a play host and while a stage is open (as `ApplyPrefab` today). |
| `ApplyPrefab {entityId}` | Write (exists) | Unchanged on the wire; now means "apply all overrides", keeping file ids. |
| `UnpackPrefab {entityId}` | Write (exists) | Unchanged on the wire; also clears the subtree's `prefabSource`. |

Additive fields:

- `HierarchyNode` and `EntityHeader`: `overridden?: bool` (this object has a modified leaf, is added, or has an added or removed component), `prefabAdded?: bool` (added to an instance). Enough for a dot on hierarchy rows without a per-row request.
- `GetEntity`'s component entries: `overrides?: string[]` (the overridden leaf paths of that component), so the inspector can bold fields with no extra call; `EntityHeader.overrides?: string[]` for header and transform leaves.
- Event: none new. `sceneChanged` already lists changed ids; a reconcile lists the objects it touched.

Policy: viewer may call `GetPrefabOverrides` (Read); the two Write commands follow the Write rules (viewer `forbidden`, `--read-only` `readOnly`); a play host refuses the Writes; both Writes join `IsRefusedWhilePrefabStageOpen` (`EditorServer.cpp:9406-9432`, which already lists `CreatePrefab`, `InstantiatePrefab`, `ApplyPrefab`, `UnpackPrefab`); not offered to Lua extensions in v1; MCP bridge tools later in the frontend repo.

Cost of the indicator: `overridden` on every hierarchy row needs the diff of every instance. The host caches the override set per instance root and invalidates it when `MarkSceneChanged` names an id under that root; a full hierarchy fetch then costs one diff per changed instance [E: linear in the instance's JSON size; a 10,000-object instance is the cap].

## 9. Engine pieces

- `engine/include/engine/prefabs/PrefabOverrides.hpp` (new): `ComputeOverrides(const GameObject &instanceRoot, const nlohmann::json &prefab) -> OverrideSet`, `Reconcile(GameObject &instanceRoot, const nlohmann::json &base, const nlohmann::json &prefab, const OverrideSet *stored) -> ReconcileReport`, `MatchLegacyShape(...)`, `SerializeOverrides`/`ParseOverrides`. Pure engine code, no editor dependency, unit-testable.
- `GameObject`/`Component`: `prefabSource` storage and (de)serialization; `GameObject::Serialize` of an instance root writes `prefabOverrides` from a callback the editor supplies (the engine alone has no base, so outside the editor the stored list is written back as it was read).
- `PrefabManager::InstantiatePrefabs`: optional id-map out-parameter; `Renumber` skips `prefabSource`.
- Editor server: base cache, reconcile hooks (open, asset change, stage close), the three commands, the additive fields, `ApplyPrefab`/`UnpackPrefab`/`CreatePrefab` changes.

## 10. Test plan

- Engine (`tests/engine/prefabs/`, next to `PrefabTests.cpp` and `PrefabInstantiationTests.cpp`): instantiate sets `prefabSource` to the file ids; duplicate of a just-created instance keeps sources (the `Renumber` skip); partial paste drops sources; diff of an untouched instance is empty; each leaf kind (header, transform, component key, `scriptData.x`, array) detected; internal reference to the corresponding object is not an override, outside reference is; root name and position never overrides, root scale is; added/removed objects and components; reconcile keeps overrides and takes the rest; new prefab child appears with fresh ids and mapped references; a removed prefab child that the instance overrode is kept with a warning; round trip of `prefabOverrides` through Serialize/Deserialize; legacy shape match and no-match.
- Host (`tests/editor-server/project/EditorPrefabTests.cpp` and a new `EditorPrefabOverrideTests.cpp`): `ApplyPrefab` keeps file ids (byte-equal file for an Apply with no changes); two instances, Apply from one, the other updates and keeps its own override; stage Save then Close updates parked instances; outside edit of the `.prefab` picked up by `PollAssets` updates the open scene as one undo step, undo/redo; `GetPrefabOverrides` shape; each `RevertPrefabOverrides` target kind; `ApplyPrefabOverrides` partial; unpack clears sources; refusals (viewer, read-only, play host, stage open, not an instance); `overridden` on `GetHierarchy`; protocol golden and test vectors at the new version; Hello capability.
- Play and runtime: a scene with instances loads in the play host and `n2server` unchanged (no reconcile at runtime); the cross-leg state hash job is unaffected.
- Frontend (FE-S4, frontend repo): Node and DOM tests for bold fields from `overrides`, row dots, revert/apply menus, legacy state.
- Docs in the same PRs (project rule): `docs/editor.html` prefab section ("copies with a link" becomes "instances with overrides"), `docs/serialization.html` (`prefabSource`, `prefabOverrides`), protocol.json and test vectors.

## 11. Risks

1. **Propagation removing user work**: the main risk. Mitigated by option C (no edit path can be missed), by keeping overridden objects that the prefab removed, by the undo step for in-session updates, and by a log line per update.
2. **An older engine saving a newer scene** drops `prefabSource` and `prefabOverrides` (unknown keys are ignored, section 2); the instances fall back to legacy matching on the next open. Documented.
3. **Stale unopened scenes in builds** (decision 3).
4. **Cost** of diffs on big instances in hierarchy refreshes; mitigated by the per-instance cache.
5. **Float noise** makes a value an override after a round trip of a gizmo; revert fixes it. Acceptable for v1.
6. **Nested prefabs** stay plain inside an instance (as the stage does); a user who expects nested propagation will not get it until v3.

## 12. Size and PR order

Size [E]: engine core (prefabSource, diff, reconcile, legacy match, tests) **L**; editor host (base cache, hooks, three commands, wire fields, Apply/Unpack changes, tests, docs) **L**; frontend FE-S4 **M-L**.

1. Engine: `prefabSource`, `Renumber` skip, instantiate id map, `PrefabOverrides` module, tests. No protocol change; scenes gain keys only for new instances.
2. Host + protocol 1.28.0: Apply/Unpack/Create changes, reconcile hooks, commands, wire fields, docs.
3. Frontend FE-S4: protocol sync, indicator, revert/apply UI.
4. Docs-only ledger and MCP entries.

## DECISIONS for the user

1. **Storage model**: self-contained instances (full data in the scene, as today) plus per-object `prefabSource` and a stored list of overridden paths, over Unity-style scenes that store only the differences and need the prefab at load. Recommendation: self-contained.
2. **How overrides are found**: diff against the prefab the host keeps the open scene reconciled with (option C), over recording on every edit path (B). Recommendation: C.
3. **When instances update**: on scene open and when a prefab file changes while the scene is open (one undo step); never at game runtime; unopened scenes update when next opened, with a later explicit "Update prefab instances in all scenes" command. Recommendation: as written, the all-scenes command as a follow-up.
4. **Legacy instances** (no `prefabSource`): match by exact shape at first open, else keep a legacy link that never updates. Recommendation: as written, no name-based guessing.
5. **Instance-owned root values**: the root's name, local position and local rotation are never overrides and never propagate (scale does). Recommendation: yes (Unity's rule).
6. **Protocol number**: 1.28.0 after EN5's 1.27.0. Recommendation: confirm; renumber if EN4 lands first.
