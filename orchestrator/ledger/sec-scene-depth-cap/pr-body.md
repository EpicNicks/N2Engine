Editor host security S2, audit finding F4: scene, prefab and snapshot JSON nested too deep crashed the host (stack overflow), and a saved deep scene set as the startup scene made it a crash loop.

## What changes
- `engine/serialization/JsonLimits.hpp` (new): `MaxHierarchyDepth = 512` GameObject levels and `MaxSceneJsonDepth = 2 * 512 + 64 = 1088` JSON levels, the string-aware nesting pre-scan (moved from `Commands.hpp`, which now re-exports it; its 64-level command-field cap is unchanged), an iterative nesting walk for values, and `IterativeDump` / `IterativeCopy` (below).
- The pre-scan runs before parsing at every scene, prefab and snapshot entry point: LoadScene, OpenScene, WritePlaySnapshot, the play-snapshot load, RestoreSceneSnapshot, the autosave restore, the edit-history `ParseStored`, and `SceneFile::Load` (which now reads the file into a string first). Error text: "the scene JSON nests more than 1088 levels deep"; nothing changes in the open scene.
- `Scene::FromJSON`, `Prefab::Deserialize` and `PrefabManager::InstantiatePrefab` also refuse a too-deep value (iterative walk), since they receive values, not text.
- `GameObject::Deserialize` counts depth (root = level 1) and throws `std::invalid_argument` naming 512 past the cap. The public signature is unchanged. Callers already turn the throw into a failed load; the resolver is local to `Scene::FromJSON` and `InstantiatePrefab`, so a failed load registers nothing in the live scene.
- `GameObject::Serialize`/`Deserialize` keep their recursion in small frames (the per-object work moved to `SerializeSelf`/`DeserializeSelf`).
- Reparent (iii): `GameObject::CanAddChild`; `AddChild` and `SetParent` refuse a move that would nest past the cap (before leaving the old parent), and `SetEntityParent` answers an Error naming the cap.
- Why 512: importers allow node trees 256 deep (`kMaxNodeDepth`), placed under scene parents; 512 keeps a margin of 256 (the floor was 288). The audit's 128 is not used.
- Docs: serialization (#gameobject-json, #design-depth-caps, #failure-handling), scenes (#loading), editor security notes, testing rows.

## Dump and copy without recursion (decided 2026-10-15: keep 512 / 1088)
The measurements below showed nlohmann's recursive `dump` and copy overflow the Windows Debug 1 MiB stack well under the cap. So scene JSON is no longer dumped or copied with them:
- `JsonLimits::IterativeDump(value, indent, indentChar, ensureAscii, errorHandler)`: byte for byte the text of the vendored 3.12.0 `value.dump(...)` for every value, indent char, ensure_ascii and error handler and any indent up to 1024, including dump's quirks (past column 512 it pads with spaces whatever the indent char; pretty binary values). Over 1024 nlohmann's own dump reads past its indent buffer (UB), so that case is not matched; no caller uses it. Open arrays/objects live on a heap stack; scalars, empty containers and keys are still written by nlohmann's own `detail::serializer` (none of them nests), so number formatting, escaping and the invalid-UTF-8 handling are dump's; pretty binary values are written by the helper itself.
- `JsonLimits::IterativeCopy(value)`: a deep copy, equal under `==`, built with a heap stack.
- nlohmann's destructor is already iterative in the vendored 3.12.0 (`json_value::destroy` flattens onto a heap vector), and its parser is iterative (`sax_parse_internal` keeps a `states` vector); both unchanged.

Sites changed:
- dumps: `SceneFileText` (scene files, autosave, play snapshot), `SnapshotScene` (undo/restore snapshots), `DumpJson` (edit-history steps, incl. whole created subtrees), `HandleSaveScene` and `HandleGetCurrentScene` responses, `Protocol::WriteJson` (every JSON a response carries), the `Scene::FromJSON` invalid-scene log text, the `ComponentSerializer` asset-list warning (logged during a scene load).
- copies: `SceneManager::UpdateScene` (LoadScene/SaveScene/SaveSceneFile store), `InstantiatePrefab` (the working copy), `InstantiateRegisteredPrefab`, `ApplyComponentState` (`withoutActive`, edit history), `LuaComponent` `scriptData` (Serialize, Deserialize, SetScriptData, SetEditorFields, the declared-fields filter in SetScript; the pending reference lambda no longer captures `j` by value, and `refValue` is a reference), `ClampFieldValue` for non-numbers, and `RegisterMember` when the member is a `nlohmann::json` (no component has one today).
- moves instead of copies: `Scene::Serialize` (`rootGameObjects`), `GameObject::SerializeSelf` (component array and entries).
- `CollectIds` and `Renumber` (prefab instantiation id walks, once per JSON level) now use a heap stack too.

Deliberately left:
- `HandleCreateScene` dump (a new empty scene), `ProjectFile` `EmptySceneText` (constant, 2 levels).
- `SetImportSettings` `customData.dump` and `SetComponentValues` `values = requested` copy: both are command fields read with `ReadBoundedJson` (64 levels).
- `EventRing::Push` dump: host-made, shallow event objects.
- Logging dumps of settings/material/model/texture/font/input bindings and `FieldInfo` limits: not scene data.
- `operator==` on two component states (the changed-keys check of SetComponentFields): recursive per level of one component's data, not of the hierarchy; documented as not covered.
- `LuaToJson`: only used for input action maps from Lua (`ActionMapJsonFromLua`), not on any scene load/save path (the earlier docs wrongly tied it to component data; corrected).
- Other components: no other component holds a `nlohmann::json` member; `ComponentSerializer`'s generic members are typed values.

## Review fixes (head after 727199d)
- LuaComponent copies of component data (above).
- Reparent results: `CreateEntityEx` under a parent at level 512 now answers an Error naming the cap before anything is made, and records no step (it used to answer EntityCreated for an object in no scene). `RestoreSubtree` (redo of a create) checks `CanAddChild` and the attach result and answers an Error, dropping the rebuilt tree. `ApplyPlacement` (undo/redo of a move) and `DuplicateEntity` check `CanAddChild` before attaching. `SetEntityParent` shares the same message (`TooDeepMessage`).
- `IterativeDump` padding/binary exactness (above), with the comment and docs stating exactly what is identical.

## Tests
Engine: `SceneDepthTest.*` (SerializationRobustnessTests; the cap tests now dump and copy with the helpers), `JsonIterativeTest.*` (new: `IterativeDump` equals `dump` for indents -1/0/1/2/4, tab indent, ensure_ascii on/off over escapes, UTF-8, int/uint/float incl. NaN/inf/-0.0/2.0, bool/null, empty containers, empty and escaped keys, a 40-level mixed chain, scalar roots, invalid UTF-8 under strict/replace/ignore; `IterativeCopy` equal under `==` and keeps number types; both at the 1088 cap and at 100 000 levels; deep indentation past column 512 with a space and a tab, binary values deep down, indents 600 and 1024 match dump, an indent of 1500 pads by the documented rule), `SceneDepthTest.ALuaComponentWithDataNestedAThousandLevelsLoadsSavesAndLoadsAgain`, `PrefabDepthTest.*`, `SceneStorageTest.SceneFileLoad*`. Editor server: `EditorSceneDepthTest.*` (LoadScene, OpenScene, WriteTextAsset then OpenScene/WritePlaySnapshot, play snapshot, autosave restore, SetEntityParent, CreateEntityEx under a parent at the cap records nothing, redo of a create whose parent was moved past the cap is an Error with no object left). Deep inputs are built iteratively as text (`tests/common/DeepSceneJson.hpp`).

No protocol change (the dumped text is byte-identical); ResolveScenePath/ResolveSceneFile and size caps untouched.

## History: the stack measurements that led to the helpers
Measured in CI (windows Debug, MSVC, 1 MiB main thread, a probe that ran each operation over a chain of N GameObjects in a child process; the probe is removed again). "ok" = survived, FAIL = stack overflow:

| operation | ok up to | overflows at |
|---|---|---|
| GameObject::Deserialize (parse text, build, free) | 512 | none (640+ refused by the cap, as intended) |
| free a chain built with AddChild (destructor recursion) | 768 | none |
| Serialize + nlohmann dump | 192 | 256 |
| Scene::FromJSON + Serialize + dump | 192 | 256 |
| copy of the parsed json (2 JSON levels per object) | 384 (768 JSON levels) | 448 (896 JSON levels) |
| InstantiatePrefab (copies the json) | 384 | 448 |
| LoadScene twice (second load goes through a json copy) | 384 | 448 |
| GetChildrenRecursive, FindChildRecursive, SceneManager add/load | 768 | none |

## Not verified
- Nothing builds locally: the helpers, their byte-identity with `dump`, and the Windows Debug 511/512-level proof are verified only by the CI run of this head.
- `operator==` on deep component data (SetComponentFields changed-keys check) is not proven on a 1 MiB stack at the 1088-level text cap (documented).
- The LuaComponent 1000-level test assumes an empty `scriptPath` round-trips without a script; only CI shows it.

## Approved deviations
Accepted by the user 2026-10-13:
- Iterative dump/copy helpers added (JsonLimits::IterativeDump, IterativeCopy), user-approved 2026-10-15; caps stay 512 / 1088
- IterativeDump uses nlohmann::detail::serializer for scalars, empty containers and keys (internal API of the vendored 3.12.0) to stay byte-identical
- Also changed beyond the measured sites: Protocol::WriteJson, the ComponentSerializer asset-list warning and the Scene::FromJSON error log use IterativeDump; ApplyComponentState copies with IterativeCopy; Scene::Serialize and GameObject::SerializeSelf move instead of copy; PrefabManager CollectIds/Renumber made iterative
- GameObject::Serialize/Deserialize split into SerializeSelf/DeserializeSelf plus a small recursive part to shrink frames
- Added GameObject::CanAddChild and refuse reparent past the cap in AddChild/SetParent plus a SetEntityParent error (scope iii done)
- Scene::FromJSON, Prefab::Deserialize and InstantiatePrefab also refuse an over-deep json value via an iterative walk (in addition to text pre-scans)
- No prefab-file text parse exists in engine/src/prefabs; prefabs arrive as json values, so the prefab entry points are covered by the value check and the depth counter
- Docs name docs/logging-and-editor.html (where the editor security notes live) as well as docs/editor.html
- Left deliberately: HandleCreateScene and EmptySceneText dumps (empty scene), customData dump and SetComponentValues copy (ReadBoundedJson 64), EventRing dump (shallow), operator== on component states (component data, documented as not covered); LuaToJson is input-action-map only, not a scene path
- Review fixes: LuaComponent scriptData copies (load/save/SetScriptData/SetEditorFields/SetScript filter, no by-value j capture), ClampFieldValue and RegisterMember<nlohmann::json> use IterativeCopy
- Review fixes: CreateEntityEx past the cap answers an Error before making anything; RestoreSubtree, ApplyPlacement and DuplicateEntity check CanAddChild/attach result; shared TooDeepMessage
- Review fixes: IterativeDump pads like nlohmann past column 512 and writes pretty binary itself; byte-identical for indent <= 1024, documented as such (over 1024 nlohmann is UB)
