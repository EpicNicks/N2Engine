# Proposal: nested prefabs and per-instance overrides

Status: **accepted** (user, 2026-11-20: decisions 1, 2 and 4 answered directly, the rest "go with your recommendation"); no code yet. Written against master at 308f4f8b (editor protocol **1.32.0**). Tags: [V] verified in the tree at that commit, [M] from memory or general knowledge, not checked here, [E] estimate. Protocol: this takes the next free minor; the working number is **1.33.0**.

Why now: the Triangle-Like port (`E:\Code\Projects\Triangle-Like N2Engine`, `LEDGER.md` "Migration to engine files") is moving its rooms and prefabs from Lua data tables to `.prefab` files, so the editor can show, edit and play them. Unity's rooms are mostly **nested prefab instances with per-instance overrides** (a crate, a turret or a pedestal placed in a room, with its scale, sprite or script fields changed). The user decided on 2026-11-20 that this becomes a general engine feature, not a game-side workaround.

This builds on `prefab-overrides.md` (EN4, design only, never coded): its `prefabSource` correspondence, its "what an override is" (section 4) and its `Renumber` and Apply fixes carry over. It **reopens EN4's decision 1** (storage model): EN4 stores self-contained instances and reconciles them only in the editor, while nesting needs propagation into files no one opens (section 3). If the user accepts this design, it replaces EN4; EN4's sections listed under "Carried over" stay binding.

## 1. Goals

1. A `.prefab` or `.scene` can hold an **instance of another prefab** (a link) with per-instance overrides, nested to any depth within the existing caps.
2. Changing a prefab **changes every instance**: in scenes and in other prefabs, opened or not, in the editor and in a running game, without rewriting any file.
3. The editor shows an instance expanded (its objects are real, selectable, editable). An edit inside an instance becomes an override. Save writes the link and the overrides, not a copy.
4. **Variants**: a prefab whose root is an instance of another prefab (a base plus overrides) works with no extra feature.
5. Files with no links load exactly as today. Old scenes with `prefabReference` instances keep loading (section 9).

Out of scope for v1: element-level merge inside arrays (an array is one override leaf, as in EN4); reordering children as an override; overrides on the asset import settings of a model instance; a Lua API to read or write overrides at runtime.

## 2. What exists today [V]

- **Instances are copies with a link.** `GameObject::_prefabReference` is an optional asset UUID on the instance root only, serialized as `prefabReference` (`engine/src/gameobject/GameObject.cpp:949-951`, read back at 1046-1048). The engine does nothing with it; the editor sets it (Create/Instantiate) and clears it (Unpack).
- **Instantiate renumbers everything** (`engine/src/prefabs/PrefabManager.cpp`): `CollectIds` gives every object and component id a fresh **random** UUID, `Renumber` rewrites every string equal to an old id and nulls a Lua `$ref` that points outside. The old-to-new map is thrown away.
- **Every scene load goes through `Scene::FromJSON`** (`engine/src/sceneManagement/Scene.cpp:771`, `Scene::Deserialize` at 744): `SceneManager` (`SceneManager.cpp:116, 229, 321`), the desktop, Android and iOS players, the web player (`web/player/WebPlayer.cpp`), `n2server` (`n2server/src/SceneLoading.cpp`) and the editor host. Every prefab instance goes through `PrefabManager::InstantiatePrefab(s)`: the editor's InstantiatePrefab, paste and duplicate, and Lua `Prefab.Instantiate` (`engine/src/scripting/bindings/LuaPrefabBindings.cpp:233`, which then sets the link, 239). **Two choke points.**
- **The prefab stage** (`HandleOpenPrefabStage`, `EditorServer.cpp:10111`) loads a file with its own UUIDs and treats nested `prefabReference` objects as plain objects with a warning. `SavePrefabStage` (10338) refuses a self-link (`prefabCycle`).
- **Unknown keys are ignored on load** (`GameObject::Deserialize` reads named keys), so new keys are backward compatible on load. An older engine **drops** them on its next save.
- **Caps**: hierarchy depth 512, scene JSON depth 1088, the editor's `MaxClipboardObjects` 10000 and `MaxClipboardBytes` 16 MiB (`docs/serialization.html`, depth caps).
- Asset UUIDs are stable: in `.import/<path>.meta`, deterministic from the project id and path, carried by a move (#287).

## 3. Storage model (decision 1)

| Option | File holds | Runtime | Propagation | Verdict |
|---|---|---|---|---|
| A. Self-contained (EN4) | full copies of every instance, plus `prefabSource` and override paths | loads files as they are | only in the editor, on open; files not open stay stale; a build ships stale data unless an "update all" pass rewrites every file | **Rejected for nesting.** One enemy edit means rewriting 41 room prefabs and every scene, and a missed pass ships old content. |
| **B. Links** (Unity's PrefabInstance [M]) | the link, the override **values**, removed ids, and objects or components the instance added | **expands** links when data is loaded (the two choke points) | automatic and everywhere: the next load sees the current prefab | **Recommended.** |

Option B's costs and their mitigation:

- **Load needs the prefab files.** Players, the web package and `n2server` already ship `assets/` and `.import/` (#287). A missing or unreadable prefab is an error per link (section 5), never a crash.
- **Load is slower.** Expansion is JSON work before deserialization, one parse per prefab, cached by UUID (section 5) [E: small next to building the objects].
- **Determinism.** The expanded tree is a pure function of the files, and ids are deterministic (section 4), so two machines with the same files build the same scene. A replay or rollback hash taken from the live state is unaffected. A project can only diverge if the prefab files differ, as it would with any asset.

## 4. The format

An object can be a **link** instead of a full object:

```json
{
  "uuid": "a1...",
  "prefabInstance": {
    "prefab": "f3c1...",
    "overrides": [
      { "source": "b2...", "path": "name", "value": "Crate (big)" },
      { "source": "b2...", "path": "positionable.localScale", "value": { "x": 2, "y": 2, "z": 1 } },
      { "source": "b2...", "component": "c4...", "path": "_halfExtents", "value": { "x": 1, "y": 1 } },
      { "source": "b5...", "component": "c9...", "path": "scriptData.speed", "value": 4 }
    ],
    "removedObjects": ["b7..."],
    "removedComponents": ["c8..."],
    "addedComponents": [ { "source": "b2...", "component": { "type": "LuaComponent", "data": { "uuid": "...", "...": "..." } } } ],
    "addedChildren": [ { "source": "b2...", "object": { "uuid": "...", "name": "Extra", "...": "..." } } ]
  },
  "positionable": { "localPosition": {}, "localRotation": {}, "localScale": {} },
  "name": "Crate"
}
```

- `source` and `component` are ids **in the linked prefab's file** (`prefabSource` in EN4's terms). For a link inside a link (a room holding a crate that holds a turret), a deeper object is addressed by its **path of sources** (`"source": "b2.../d4..."`, outer to inner), as Unity's `fileID` chains do [M].
- The link's own `uuid`, `name` and `positionable` are the **instance-owned values** (EN4 section 4, decision 5): the root's name, local position and local rotation, plus its parent and sibling place. They are kept on the link and never propagate. The root's scale and everything else are ordinary leaves.
- Leaves: as EN4 section 4 (object header `name`/`tag`/`layer`/`isActive`; the three transform parts; each top-level key of a component's `data` except `uuid`; each inner key of a container field, `scriptData.<key>`; an array is one leaf).
- **Ids are deterministic**: an object or component made from source id `s` under a link with id `L` gets `UUIDv5(L, s)` (the same namespace scheme as asset UUIDs). Applied recursively (a nested link's own id is derived first), every expansion of the same file gives the same ids. So:
  - a `$ref` from outside into an instance survives a save and reload (it names a stable derived id);
  - the editor can rebuild an instance after a prefab change and every reference to its objects still resolves (section 7);
  - two instances of one prefab never collide (their `L` differ).
  `InstantiatePrefab(s)` keeps random ids for a **new** instance's link id `L` (two spawns stay distinct), then derives the rest from it.
- References inside the linked prefab are mapped as `Renumber` maps them today, through the derived ids. An override value holding a reference names an id **as the instance sees it** (a derived id, or an object outside the instance), and is kept as written.
- `prefabReference` (today's link) remains for instances that are not links (legacy copies, section 9).

## 5. Expansion (engine core)

`engine/include/engine/prefabs/PrefabNesting.hpp` (new), pure engine code with no editor dependency:

- `Expand(json &objectJson, PrefabSource &source, ExpandReport &report)`: replaces every link in a GameObject JSON tree, depth first, with the full object tree.
  1. Load the prefab's `rootObject` by UUID through a `PrefabSource` interface. The default reads `PrefabFile` through `ResourceLoader`, with a per-call cache by UUID. Tests and the editor pass their own (the editor's sees unsaved stage content).
  2. Expand links inside the prefab first (recursion with a **visited stack**: a link to a prefab already on the stack is a cycle, reported and replaced by an empty object named after the link, never a crash).
  3. Derive ids (section 4) and map internal references.
  4. Apply `removedObjects`, `removedComponents`, then `overrides` (by source path), then `addedComponents` and `addedChildren`.
  5. Put back the instance-owned values and mark the result: the root gets `prefabReference` = the prefab UUID, and every object and component gets `prefabSource` (EN4 section 3) and a hidden `prefabLink` id (its link's `L`).
- Called by `Scene::Deserialize` before the roots are built, and by `PrefabManager::InstantiatePrefabs` before `CollectIds`. So the players, web, `n2server`, Lua `Prefab.Instantiate`, paste and duplicate all expand with no change of their own.
- **Errors are per link**, logged with the file and link id:
  - a missing or unreadable prefab: the link becomes an empty object with its instance-owned values and name, and the error names the UUID;
  - a cycle;
  - an override naming a source the prefab no longer has: dropped with a warning (it is also dropped on the next editor save);
  - a value of the wrong JSON shape: the component's own `Deserialize` decides, as for any malformed field.
- **Caps**: the expanded tree is checked against the hierarchy cap (512 levels) and the editor's object cap (10000), and expansion depth (links within links) is capped at 64 [E]. Past a cap the link fails as above.
- `ExpandReport` counts links, prefabs loaded, cleared references and errors, for one summary log line per load.

`Collapse(const GameObject &instanceRoot, const json &expandedPrefab) -> json` is the inverse. It diffs the instance against the prefab expanded the same way (EN4 section 4, comparison rules: internal references mapped back through `prefabSource`, outside references always overrides, floats exact) and writes the link. `GameObject::Serialize` of an object that carries `prefabLink` calls it through a `PrefabSource` the caller supplies. **Without one (a game at runtime), the object is written expanded**, as a legacy copy with `prefabReference`, so a runtime save never loses data.

## 6. Editor host

The host keeps instances expanded in memory. They are ordinary objects, so every existing command (select, inspect, gizmos, picking, undo, copy and paste) works on them unchanged. What changes:

- **Open** (`HandleOpenScene` 11521, `HandleOpenPrefabStage` 10111, autosave restore): expansion happens in `Scene::FromJSON` and the stage's load. The stage now expands nested links instead of treating them as plain objects with a warning.
- **Save** (`HandleSaveScene` 5703, `SavePrefabStage` 10338, autosave, `WritePlaySnapshot` 6306): serialize with the host's `PrefabSource`, so instances collapse to links. The play snapshot may stay expanded (the play host would only expand it again); recommended to collapse it, so play loads exactly what a player loads.
- **Overrides are found by diff**, never recorded per edit path (EN4 section 5, option C): any edit, undo, paste or extension write is seen.
- **Propagation** (a `.prefab` changes: the host's own Apply or stage Save, or an outside edit seen by `PollAssets`): every instance of that prefab in the open scene, at any depth, is **rebuilt**. Collapse it with the old prefab, then expand it with the new one. Deterministic ids keep every reference to its objects valid, so rebuilding is safe; EN4 had to reconcile in place for lack of this. It is one undo step, "Update instances of X", through `SnapshotScene`. While a stage is open the parked scene rebuilds at Close.
- **CreatePrefab** of a subtree that contains instances writes them as links. **ApplyPrefab** writes the instance's overrides into the file, keeping file ids (EN4 section 3), then propagates. **UnpackPrefab** replaces a link by a plain copy (clears `prefabReference`, `prefabSource`, `prefabLink` in the subtree); "Unpack completely" is the same applied to nested links too.
- **Cycles**: `SavePrefabStage` and `CreatePrefab` refuse a save that would make a prefab contain itself at any depth (extends today's `prefabCycle`).

### Commands and wire (protocol 1.33.0, working number)

Hello capability `prefabNesting`. All additive:

| Command | Class | Meaning |
|---|---|---|
| `GetPrefabOverrides {entityId}` | Read | As EN4 section 8, for the innermost link containing the entity, plus `linkPath` (outer to inner) so a client can show nested levels. |
| `RevertPrefabOverrides {entityId, targets}` | Write | As EN4 section 8. |
| `ApplyPrefabOverrides {entityId, targets, level?}` | Write | As EN4 section 8. `level` picks which prefab in the link path receives the overrides (default: the innermost), as Unity's "Apply to Prefab X" [M]. |
| `UnpackPrefab {entityId, completely?}` | Write (exists) | `completely` also unpacks nested links. |

Additive fields: `HierarchyNode`/`EntityHeader` get `overridden?`, `prefabAdded?` (EN4) and `linkDepth?` (how many links the object is inside, for indentation or colouring). `GetEntity`'s components get `overrides?: string[]` (EN4).

Policy: as EN4 (viewer may Read; Write rules; a play host refuses Writes; all three join `IsRefusedWhilePrefabStageOpen` except Get). MCP bridge tools and the Lua extension API come later, in the frontend repo.

## 7. Carried over from EN4 (binding if this is accepted)

Section 3 (`prefabSource` on objects and components, the `Renumber` skip of `prefabSource`, `ApplyPrefab` writes file ids, `UnpackPrefab` clears sources); section 4 (leaves, structural changes, instance-owned values, comparison); section 5 option C (diff, not recording); the test plan's engine and host cases (section 10), adapted to links. **Dropped from EN4:** the stored `prefabOverrides` path list and the in-place `Reconcile` (section 6 there), which links and deterministic ids replace.

## 8. Frontend (frontend repo, outline)

- Hierarchy: prefab rows marked as today, nested ones by `linkDepth`, and a dot on `overridden` rows.
- Inspector: bold overridden fields (`overrides`); per field and per component "Revert" and "Apply to <prefab>" (one item per level of `linkPath`); "Open prefab" for each level.
- Protocol sync and DOM tests, as earlier frontend syncs.

## 9. Old data

- **Files with no links**: unchanged.
- **Legacy instances** (`prefabReference` with a full copy, written by 1.19.0 to 1.32.x): they load as copies, as today. They are never rebuilt or collapsed. The inspector offers "Convert to link": EN4 section 7's shape match finds the correspondence, the diff against the current prefab becomes the overrides, and the copy becomes a link. Nothing converts on its own.
- **An older engine** reading a file with links ignores `prefabInstance` and builds an empty object (the link's own keys). Saving would write that. Documented: open such a project only with an engine that has `prefabNesting`.

## 10. Test plan

Engine (`tests/engine/prefabs/`):
- expansion of one link, nested links, a variant (root is a link);
- each override kind (header, transform part, component key, `scriptData.x`, array); removed and added objects and components; overrides addressed through a source path two links deep;
- deterministic ids (the same file twice gives the same ids; two links to one prefab differ; a `$ref` from outside into an instance survives a round trip);
- `InstantiatePrefab` of a prefab containing links (fresh `L`, derived inner ids);
- cycles (direct and indirect), a missing prefab, a stale override source, and the caps; each fails as section 5 says, without throwing;
- `Collapse` is the inverse of `Expand` (expand then collapse an untouched instance gives the link with no overrides; byte-equal after a second round trip);
- a runtime `Serialize` without a `PrefabSource` writes expanded copies;
- Lua `Prefab.Instantiate` of a nested prefab; `Scene::FromJSON` used by the desktop player, `n2server` and the web player sees expanded objects.

Host (`tests/editor-server/project/`): open and save round trip byte-equal; an edit inside a nested instance saves as an override on the right link; propagation after Apply, after stage Save, and after an outside edit (one undo step, references into the instance still resolve); `ApplyPrefabOverrides` with `level`; Revert kinds; `UnpackPrefab completely`; cycle refusals; legacy instances untouched; protocol golden and test vectors at 1.33.0; the Hello capability.

Runtime: the cross-leg state hash job unchanged for a scene with no links; a scene with links gives the same hash on every leg.

Docs in the same PRs: `docs/serialization.html` (Prefabs: links, expansion, ids, errors), `docs/editor.html` (prefab section), `docs/scripting-lua.html` (Prefab.Instantiate of nested prefabs), protocol.json and test vectors, and the docs search index.

## 11. Risks

1. **A missing prefab file** breaks instances at runtime. Mitigation: per-link errors and an empty placeholder. Also a project scan (later) listing links to missing prefabs, for the build targets' scan.
2. **Load cost** in big scenes with many links: the per-call cache, and expansion is JSON-only. Measure on the Triangle-Like rooms (41 rooms, about 80 prefabs).
3. **An older engine saving a newer file** drops the links (section 9).
4. **Override paths rot** when a prefab's components are removed: dropped with a warning on load, and gone on the next save.
5. **Float noise** turns a value into an override after a gizmo round trip (EN4 risk 5).

## 12. Size and PR order

Size [E]: engine core (format, `Expand`, `Collapse`, ids, errors, tests, docs) **L**; editor host (save collapse, stage expansion, propagation, three commands, wire fields, tests, docs) **L**; frontend **M-L**.

1. **Engine core**: `PrefabNesting` (Expand, Collapse), `prefabSource` and `prefabLink`, the `Renumber` skip, the two choke points, the runtime "write expanded" rule, tests and docs. No protocol change. Until PR 2 lands, the editor host refuses to save a scene or stage containing links (`prefabNestingUnsupported`), so no link is ever lost by a save.
2. **Editor host + protocol 1.33.0**: save collapse, stage expansion, propagation, Apply/Unpack/Create changes, the commands and fields, docs.
3. **Frontend**: protocol sync, indicators, revert/apply UI, "Open prefab".

The Triangle-Like port needs PR 1 to run from `.prefab` files and PR 2 to edit them in the editor. Its converter writes links directly (`tools/unity2lua.py`), so it does not wait for the frontend.

## DECISIONS for the user

1. **Storage model**: links expanded at load (B), which replaces EN4's self-contained model (A). Recommendation: B.
2. **Ids**: deterministic `UUIDv5(link id, source id)` for everything inside a link; random only for a new instance's link id. Recommendation: yes.
3. **Instance-owned root values**: the root's name, local position and local rotation never propagate (EN4 decision 5). Recommendation: yes.
4. **Runtime saves** (a game calling `Serialize` with no prefab source) write expanded copies with `prefabReference`, never links. Recommendation: yes; collapsing needs the editor's view of the prefabs.
5. **Play snapshot**: written collapsed (play loads what a player loads). Recommendation: collapsed.
6. **Legacy instances**: stay copies; an explicit "Convert to link" per instance, never automatic. Recommendation: yes.
7. **PR split**: engine core first, with the host refusing to save links until PR 2. Recommendation: as written.
8. **Protocol number**: 1.33.0. Confirm; renumber if another minor lands first.

## Decisions taken (user, 2026-11-20)

1. Storage: **links expanded at load** (B). Replaces EN4's self-contained model.
2. Ids: **deterministic** `UUIDv5(link id, source id)`; random only for a new instance's link id.
3. Instance-owned root values: as recommended (name, local position, local rotation never propagate).
4. Runtime saves: **expanded copies** with `prefabReference`. Asked which is faster: expanded is (one serializer walk, no prefab reads, no diff, and the stored JSON needs no expansion when reloaded); links only save size, on paths that are neither hot nor large (`SceneManager::AddScene`'s stored copy, `RegisterPrefab`).
5. Play snapshot: collapsed, as recommended.
6. Legacy instances: stay copies, explicit "Convert to link", as recommended.
7. PR split: **engine core first**, the host refusing to save links until PR 2, as recommended.
8. Protocol: 1.33.0, renumbered if another minor lands first.
