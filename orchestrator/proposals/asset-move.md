# Moving assets between folders from the editor: design proposal

Status: proposal, no code. Written against master at a0472d54, editor protocol 1.23.0. Line numbers below are for that commit.

The Assets panel cannot drag a file or a folder onto another folder. New scripts, scenes and prefabs are created in the selected folder, but nothing moves an existing asset, and the protocol has no command for it.

## 0. The finding that shapes everything: a move changes the UUID

`docs/editor.html:818` says `MoveAsset` stays post-MVP because "a move changes the asset's UUID, which needs a rewrite of every reference". The code confirms it, and it also shows that the usual premise "the .meta moves with the file and keeps the UUID" does not hold here:

- An asset's UUID is not stored as an identity: it is derived from the res:// path. `ResourceUUID::FromPath` is `GenerateNameBased(projectNamespace, path.ToString())` (`engine/src/io/ResourceUUID.cpp:56-65`); a sub-asset's is `FromSubAsset(parentPath, key)`, derived from the parent PATH plus the key (`ResourceUUID.cpp:67-77`).
- The loader enforces it. When a .meta is read and its `uuid` differs from `FromPath(path)`, the loader logs "UUID mismatch ... Regenerating" and overwrites the .meta's uuid (`engine/src/io/ResourceLoader.cpp:329-337`). A new file gets `FromPath` too (`ResourceLoader.cpp:363`).
- The .meta is not beside the file. It lives in `<project>/.import/<same relative folder>/<name>.meta` (`ResourceLoader.cpp:107`, `GetMetadataPath` at `382-389`) and is committed (`.n2/` is the ignored per-machine folder, `ProjectFile.cpp:617`). It stores `uuid`, `resourcePath`, `resourceType` and `customData` (import settings, the model sub-asset index) (`engine/src/io/AssetMetadata.cpp:22-30`, `SaveToFile` just below).
- The .meta's `resourcePath` is read but never re-derived in the scan: in `CreateOrUpdateMetadata` the existing-meta branch (`ResourceLoader.cpp:323-360`) never assigns `meta.resourcePath`, and `ScanDirectory` keys `_metadata` by it (`ResourceLoader.cpp:272-273`). So a .meta copied by hand to a new `.import` location without rewriting its `resourcePath` would be indexed under the OLD path. A move must rewrite the .meta, not just rename it.
- Today a rename by the file system is seen as delete plus add: the old path is dropped from `_metadata`, `_uuidToPath` and the caches but "Their .meta files stay" (`ResourceLoader.cpp:166-199`); the file at the new path gets a brand new .meta and UUID, and import settings are lost.

So "keep the UUID" is not free. Two ways to get references to survive (section 2): make UUIDs stable (invasive, and unsafe, see 2.3), or keep UUIDs path-derived and teach the loader that the new asset answers to its old UUIDs and old path (recommended).

## 1. Command shape

### 1.1 MoveAsset

- **Id 0xAF** (the next free id in the 0xA0 block; `Protocol.hpp:95-109` uses 0xA0-0xAE) and a new response type (`AssetsMoved`, id 0xA5, the next free; response ids in use are 0xA0-0xA4, `Protocol.hpp:185-191`).
- **Request**: `json moves`, an array of `{from, to}` res:// paths, 1..256 entries. `to` is the new full path of the asset (not the destination folder), so the same command also renames. The frontend computes `to = destinationFolder + "/" + basename(from)` for a drop. A list of `{from, to}` is preferred over "sources plus destination" because it is strictly more general (rename comes free; the drop UI is a one-line mapping) and it makes the response a 1:1 echo.
- **Files and folders**: `from` may be a file with a loader (an indexed asset) or a folder. A folder moves as one directory rename plus a rewrite of every indexed .meta under it. Files with no loader (a `.txt` in assets) move with their folder; moving a single unindexed file is refused (`Not an asset`), as `SetImportSettings` does (`EditorServer.cpp:7923`).
- **Response** `AssetsMoved`: one JSON field `result = {moved: [{from, to, uuid, previousUuid, type}], warnings: [string]}`. For a folder, `moved` lists the folder entry and each indexed asset under it (capped at 10000 assets per call, else Error before anything is touched). `previousUuid` is the UUID the asset had at `from`.
- **Since**: 1.24.0. It is the next free minor, but minors are taken serially: whichever protocol PR merges first takes 1.24.0 and the next one rebases onto 1.25.0. Check `Protocol.hpp:14` and `protocol.json:2` at merge time.
- **Class, roles, read-only, play host**: register `{CommandType::MoveAsset, "MoveAsset", CommandClass::Write}` beside `CreateFolder` (`EditorServer.cpp:4276`) and add it to `IsEditOnlyCommand` (`EditorServer.cpp:5975`). Nothing else is needed:
  - read-only host: `IsReadCommand` is an allow list, so an unclassified-as-read command is refused with the readOnly error before the handler runs (`EditorServer.cpp:4421-4427`);
  - roles: a viewer is refused (Write class), editor and admin are allowed (`RoleAllows`, `EditorServer.cpp:2324-2341`), answered with ErrorCoded forbidden;
  - play host: refused by `IsEditOnlyCommand` (`EditorServer.cpp:4442-4446`).
- **Audit line**: the generic `audit: user=... role=... command=MoveAsset` line comes for free (`EditorServer.cpp:4451-4459`), and it names no path (as for CreateFolder). Add one `ServerLog::Info` per call, "Moved N assets (first: res://a -> res://b)", with the paths passed through `SanitizeForLog`, as `HandleCreateFolder` does ("Created folder", `EditorServer.cpp:8151`).
- **Not polled**: it is a write, it logs.
- **MCP bridge** (it is in the frontend repository, not this one; `docs/mcp.html:95`): a write tool `n2_move_asset` is warranted, because an agent that creates scripts and scenes needs to put them in the right folder and today can only create at a path. Tier: an ordinary write tool (listed for an editor on a host that is not read-only), NOT an irreversible one: a move is undone by moving back, and with the redirect design of 2.2 the round trip restores the original UUID. It is not inside an "AI: tool #n" edit group, which exists for scene history; a file move does not touch scene history. Keep the bridge's existing path policy (plain `res://`, no `..`, first segment not `editor`, never `.meta`; `docs/mcp.html:174`), cap an agent call at 50 moves, and surface `warnings` to the model as data. Since it changes what a scene resolves to on disk, I would list it only after the frontend UI has shipped and the protocol sync (bridge PR last, section 7).

## 2. What a move must keep, and what it breaks

### 2.1 Every place a path or a UUID is stored

By UUID (survive a move only if UUIDs resolve through the move):

| Place | Evidence | Breaks without help? |
|---|---|---|
| Component asset references (meshes, materials, textures, audio ...) in .scene and .prefab JSON | saved as `assetRef->GetUUID().ToString()`, resolved by `ResolveAssetReference` -> `Resources::LoadByUUID` (`ComponentSerializer.hpp:360-395`, `:403-410`) | Yes: the old UUID is no longer indexed, "Asset ... not found", reference null |
| LuaComponent script in scenes and prefabs | `j["scriptUUID"] = loader.GetUUID(_scriptPath)`; `path` fallback only when the UUID is ZERO (`LuaComponent.cpp:1375-1387`); load: UUID -> `GetMetadata(uuid)`, else warn and `_hasMissingScript = true` (`LuaComponent.cpp:1405-1421`) | Yes, silently: the script component shows as a missing script |
| Material (.mat) textures | a texture is a UUID string, else a path (`Material.cpp:127-137`) | UUID form breaks; path form breaks (below) |
| Scene UUID | the scene's own UUID is `FromPath(its file)` (`EditorServer.cpp:9291, 9347, 9404`; `n2server/src/SceneLoading.cpp:82`) | A moved scene gets a new scene UUID. Anything keyed by it (autosave names are path based, a client's scene tab) changes |
| Model sub-asset UUIDs (mesh, material, texture parts) | `FromSubAsset(parentPath, key)` (`ResourceUUID.cpp:67-77`); the index is in the parent's .meta customData (`ResourceLoader.cpp:686-740, 1038`) | Yes: every part UUID changes with the parent path |
| Saved edit history and autosave | `EditHistory` restores scene snapshots as JSON (UUIDs inside); `.n2/autosave/` holds scene JSON (`docs/editor.html:439`) | Undo past a move would restore old UUIDs |

By PATH:

| Place | Evidence | Breaks? |
|---|---|---|
| `project.n2proj` `startupScene` and `scenes` | res:// paths (`ProjectFile.hpp:24, 45-46`; `ProjectFile.cpp:223-229, 316`; `SetStartupScene` normalises and saves, `EditorServer.cpp:9839-9867`) | Yes: the host would open nothing or refuse the project (`startupScene ... not a res:// path to a .scene file` is only a shape check, a missing file is found at open time) |
| Lua `require("scripts.util")` | module name is the path with dots, `+ ".lua"`; `Load<LuaScript>(path)`; modules keyed by name (`LuaRuntime.cpp:651-668`, `PathToModuleName` `LuaRuntime.cpp:~685`) | Yes, at run time, and only inside script text; the host cannot know without reading the text |
| `LuaRuntime::ReloadModule` and the declaration cache are keyed by module name / path (`LuaRuntime.cpp:787-830`, `ScriptDeclarationCache`) | | Stale entries under the old name |
| A `res://...` string in a script's own data or Lua source (`Resources.Load("res://...")`, `scriptData` strings, `res://models/x.glb#mesh/Body` sub-asset references, `LuaComponentBindings.cpp:175-200`) | | Yes, if stored as a path |
| .mat texture given as a path (`Material.cpp:127-137`: `res://` or relative to the material's folder) | | A res:// path breaks; a path relative to the material's folder breaks if only one of the two files moves (and works if both move together) |
| `scriptPath` fallback in old scenes (`LuaComponent.cpp:1424-1432`) | | Breaks |
| Import settings (.meta `customData`) | per file, keyed by the file; texture/model/font settings hold no paths, and the model sub-asset index holds `{key: {type, uuid}}` (`ResourceLoader.hpp:158-166`) | The settings travel with the .meta; the sub-asset UUIDs inside the index must be recomputed for the new parent path |
| Editor-side state: the frontend's open script tabs, the selection, thumbnails by UUID; `OnAssetsChanged` hooks | | Refreshed through the event (section 4) |
| Open scene path, scene dirty state | `_openScenePath` (`EditorServer.cpp:9165-9169, 9217, 9407`) | See 3 |

### 2.2 Options

1. **Refuse any move that would break a path reference.** Not feasible as the only mechanism: UUID references are everywhere and scene files are the common case; refusing "everything referenced" means almost nothing can move, and Lua/text path strings cannot be found reliably.
2. **Rewrite the references** (scan every .scene, .prefab, .mat for the old UUIDs and old path, write the files back). Correct for the files it reaches, but: it rewrites many files per move (VCS churn), cannot update the open scene's unsaved edits, the undo history and autosaves (they hold old UUIDs), cannot reach Lua text, and a crash half way leaves a project half rewritten with no undo. Also it must understand every serialiser's reference form.
3. **Warn only.** Honest, cheap, and leaves every scene broken after the first drag. Unacceptable for UUID references.
4. **Make the moved asset keep answering to its old identity (redirect; recommended).** The engine learns that an asset moved: the .meta records `movedFrom`, the list of the res:// paths it has had (oldest first, capped at 16). The loader derives from it, at scan time, everything that used to resolve:
   - `FromPath(oldPath)` for each entry is an alias UUID for the asset; `FromSubAsset(oldPath, key)` for each sub-asset key is an alias for the sub-asset. `GetMetadata(uuid)`, `LoadByUUID` and `GetCachedByUUID` look at the live UUID map first and the alias map second (`ResourceLoader.hpp:100-113`, `_uuidToPath` at `:251`).
   - A res:// path that is no longer indexed but is in some asset's `movedFrom` resolves to that asset: used by `Resources::FindProjectPath`/`Load` (`engine/src/io/Resources.cpp:45-51`), the `scriptPath` fallback and the `res://x.glb#key` form, so path strings in scenes, materials and scriptData keep working too.
   - A live asset always wins over an alias. When a new file appears at a path that equals an alias (someone creates `res://old/name.png` after moving the original away), the loader drops that alias and logs a warning, so a UUID is never ambiguous.
   - References heal themselves: the next save of a scene writes `GetUUID()` of the asset, which is the new UUID. Undo history and autosaves keep working, because old UUIDs still resolve.
   - A move back removes the entries equal to the new path from `movedFrom`: the round trip restores the original UUID exactly.
   - What the redirect does NOT cover, and the host handles or warns about:
     - `project.n2proj` `startupScene` and `scenes`: the host rewrites them in the same call through `SaveProject` (the path `SetStartupScene` uses, `EditorServer.cpp:9862`). Not left to the redirect, so the file is correct for tools that read it without the engine (n2server, the CI smoke).
     - Lua `require("old.module")` by name: not redirected (a module name is an identity inside the Lua state; redirecting would make two names for one module). Warned: the host scans `.lua` files (bounded, at most 2000 files of at most `MaxTextAssetBytes`) for the old module name in quotes and for the literal `res://` old path, and reports `warnings` such as `res://scripts/a.lua still names scripts.util`. Non-blocking.
     - A path string in non-text data (a binary), and `res://` strings assembled at run time: unknowable.

   Recommend option 4, with the host's `project.n2proj` rewrite and the warning scan. A later optional `rewriteReferences: true` flag (a trailing request field, a later minor) could additionally rewrite scene/prefab/material files on request; not in v1.

### 2.3 Why not make the UUID itself stable

The alternative is to make the .meta's `uuid` authoritative (remove the "UUID mismatch, regenerating" rule at `ResourceLoader.cpp:329-337`). It looks simpler and is worse:

- Two files could then have the same UUID: move `a.png` to `b/a.png` (it keeps `FromPath(a.png)`), then create a new `a.png`, which gets `FromPath(a.png)` too. Preventing that needs a collision detector and a rule on who gets a new UUID, which is the alias design in disguise.
- Sub-asset UUIDs are `FromSubAsset(parent path, key)`; stable parent UUID does not give stable sub-asset UUIDs without storing them all.
- The "same .meta in two clones without .import" determinism (a clone with no `.import` computes the same UUIDs) would be gone.
- Scene UUID = `FromPath` (`EditorServer.cpp:9291`) would stay path-bound anyway.

### 2.4 Engine API for the host

A new `ResourceLoader::MoveAsset(const ResourcePath &from, const ResourcePath &to)` (and a folder variant that takes the list of indexed assets under a prefix) that: writes the new .meta (new `resourcePath`, new `uuid = FromPath(to)`, same `resourceType` and `customData` with the sub-asset index recomputed for the new parent, `movedFrom = old movedFrom + from`); re-keys `_metadata`, `_uuidToPath`, `_assetState`, `_subAssets` and the caches; and registers the aliases. `AssetMetadata` gets a `movedFrom` vector, read and written as a top-level `movedFrom` key; an engine older than this one ignores the key on read and drops it the next time it rewrites that .meta, which only loses the redirect, never breaks the asset. This is the single place that owns the .meta, as `SetImportSettings` and `SetSubAssetIndex` are today (`ResourceLoader.hpp:131-166`).

## 3. Edge cases

**Order of operations per entry, crash safe** (no undo exists for file operations):
1. Validate everything first, touching nothing (below). Any failure: one Error, nothing changed.
2. Write the NEW .meta atomically (`WriteTextFileAtomically`, as `SaveToFile` does).
3. `rename` the file (or the folder).
4. Remove the OLD .meta (or the old `.import` subtree).
A crash after 2 leaves an extra .meta, harmless (stale .metas are kept by design, `ResourceLoader.cpp:167`); a crash after 3 leaves the file moved, the new .meta valid, and an old .meta that is stale; at no point does a file sit at a path with a .meta that has lost its import settings.

**Partial failure and atomicity.** All-or-nothing in the validation phase (every check below runs for every entry first). In the execution phase the host keeps a journal of completed steps; if entry k fails (the file is open by another process on Windows, a permission error, a full disk), it undoes entries k-1..1 in reverse (rename back, delete the new .meta) and answers a single Error naming the entry that failed and the reason (through `ClientText::Failure`, as `HandleWriteTextAsset` does, `EditorServer.cpp:8054-8057`). If a rollback step itself fails, the Error says so and lists the entries left half-moved, and the host logs it as an error; the next watcher rescan then reconciles the index with the disk (files exist, so they are indexed afresh). No success is reported for a call that rolled back, and no event is sent for it.

**Validation (all before any change):**
- Paths: `ResolveAssetPath` on `from` (must exist) and `to` (`EditorServer.cpp:7517-7591`): res:// only, valid names on every platform (`IsValidAssetName`: no `<>:"|?*\`, no trailing dot/space, no Windows device names, at most 255 bytes), inside the assets folder, symlinks cannot lead out. A source that is itself a symlink: refused (as `ListAssets` skips links, `EditorServer.cpp:7771`).
- `to` is not the assets root. The parent of `to` must exist (as `WriteTextAsset`, `EditorServer.cpp:8049-8053`) or the call creates it (recommended: create, like `CreateScriptAsset`, `:8097`; a drop never needs it, an agent might).
- A file keeps its extension (a move that changes the extension is refused; renaming a `.png` into `.lua` is a different operation).
- **Name collision at the destination:** refuse, never overwrite (no undo). The check is case-INSENSITIVE on every platform, so a move that works on Linux cannot create `A.png` next to `a.png`, which would not check out on Windows or macOS. A stale .meta (no file) at the destination is replaced by the moved asset's.
- **Folder into itself or a descendant:** refuse when `to == from` or `to` starts with `from + "/"`, compared on the resolved, case-folded paths. Also refuse a list in which one source is inside another, or two entries have the same `to`.
- **Same place:** `from == to` is an Error ("already there"); the frontend filters such rows before sending.
- **The open scene:** refuse a move of the open scene's file, or of a folder that contains it, as `WriteTextAsset` refuses to write it (`EditorServer.cpp:8038-8042`, `OpenScenePath()` at `:9165`). Moving it would orphan `_openScenePath`, the scene's UUID (path-derived), the dirty/saved state and the autosave file name. The message says "save the scene elsewhere with SaveSceneToFile first". (Allowing it, with `_openScenePath` updated, is DECISION 4.)
- **Moves under `res://editor`:** `WriteTextAsset` has no special rule for the extension folder (I found none in `editor-server/src`), so v1 keeps parity. The bridge already blocks `editor/` for an agent.
- **Windows path length:** refuse when the destination's absolute path would pass 240 characters on Windows (`MAX_PATH` minus headroom; the project does not enable long paths).
- **Counts:** at most 256 entries and 10000 indexed assets per call.

**A script currently loaded in memory.** The watcher's reload works by path: `ReloadLoadedAsset` sets the new source into the cached `LuaScript` and calls `LuaRuntime::ReloadModule(path, script)` (`EditorServer.cpp:7593-7616`), and a removed path drops its cache entries while "whoever holds the asset keeps it" (`ResourceLoader.cpp:183-192`). For a move that would leave every `LuaComponent` with `_scriptPath` = the old path: its next save writes `scriptPath` = old (the UUID lookup of a path that is no longer indexed is ZERO, `LuaComponent.cpp:1375-1387`) and its diagnostics name the old file. So the host, after the loader has moved the asset, re-points the open scene's LuaComponents whose script is under the moved set to the new path with a new engine call, `LuaComponent::RetargetScript(newPath)`: it changes `_scriptPath` and the module bookkeeping (erase the old module name in `_loadedModules`, forget the old declaration-cache entry, `ReloadModule` the new path) and does not run `OnDestroy`/`OnCreate` or lose `_scriptData`. The loader's cache entry is moved to the new path/UUID. The simple and safe fallback if `RetargetScript` is judged too big for v1: drop the caches as a removal does, and re-resolve each component's script through the redirect on next scene load; but then the open scene saves a stale `scriptPath` until reloaded, so I recommend `RetargetScript`.

**The watcher seeing delete + add.** The host does the whole change itself and then calls the loader's refresh for what it touched, exactly as `NoteAssetWritten` does after a write (`EditorServer.cpp:7662-7683`): `_metadata` is already updated when the watcher's next `AssetsChangedOnDisk` fires (`:7694`), so the rescan finds nothing new for those paths and sends no duplicate event (the existing test `TheWatcherDoesNotRepeatWhatACommandAlreadyReported`, `EditorAssetTests.cpp:1093`, is the model). A move done OUTSIDE the editor (git, the OS) still arrives as removed + added without `moved`: no UUID continuity is possible there; a content-hash pairing in the watcher is a possible later improvement, not v1.

**Case-only renames** (`Texture.PNG` -> `texture.png`, a folder `Props` -> `props`). On Windows and default macOS `exists(to)` is true for the same file, and `ResolveAssetPath` canonicalises an existing target to the spelling the file system has (`EditorServer.cpp:7576-7588`), which would silently turn the new spelling back into the old one. The handler must resolve `to` lexically (not through the existing-target branch) when `std::filesystem::equivalent(from, to)`, treat it as a case-only rename and perform it as two renames through a temporary name in the same folder (journal both steps). On Linux it is an ordinary rename. UUIDs change either way (the path string is case sensitive), the redirect covers it.

**Windows versus Linux.** A folder rename fails on Windows if any file below it is open in another process (an antivirus or a viewer); that is the "file in use" rollback case above. The host itself holds no handle to an asset file between commands (the loader reads and closes). Rename never crosses volumes here (`.import` and `assets/` are under the project root; if `.import` is a mount point the rename fails and rolls back, reported as such).

**Import settings of the old location.** An orphaned old .meta at the destination: replaced (above). The old .meta at the source: removed (step 4), not kept, so a later file put at the old path starts clean (it will get no import settings, which is correct: it is a different file).

## 4. Events and the frontend refresh

The host sends the existing `assetsChanged`, with one new key:

```json
{"added": ["res://textures/hero.png"],
 "removed": ["res://hero.png"],
 "modified": [],
 "moved": [{"from": "res://hero.png", "to": "res://textures/hero.png",
            "uuid": "<new>", "previousUuid": "<old>"}]}
```

- A moved asset is listed in `moved` AND in `removed` (its old path) and `added` (its new path). A client that ignores `moved` (the current frontend, any 1.23 client) refreshes correctly with no change: it sees a file leave and a file arrive. This is the compatibility reason; the event is JSON, and "a client ignores kinds and keys it doesn't know" (`protocol.json` PollEvents description).
- A folder move lists the folder's indexed assets individually (not the folder entry, which is no asset), capped like the hook lists (`ExtensionHost::MaxHookAssetPaths = 1000`, `ExtensionHost.hpp:183`); beyond the cap the event still carries the capped list and clients refetch with `ListAssets` anyway.
- The response `AssetsMoved` carries the same pairs, so the issuing client can update without waiting for the event; other clients get the event.
- `OnAssetsChanged` hooks (`EditorServer.cpp:7644-7659`) keep receiving added/removed/modified (the moved assets show as removed and added) and get `moved` as a fourth, capped list.
- The ListAssets folder list comes from the file system (`EditorServer.cpp:7761-7787`): moving a folder shows it in the new place at the next `ListAssets`. Because an empty-folder move changes no indexed asset, the event can be empty; the response and the frontend's own refetch after the call cover that.
- **Frontend refresh** (outline; the frontend is a separate repository): on the response or on an event with `moved`: refetch the folders it shows (`ListAssets`), remap the selection and the expanded-folder state from old to new paths, retarget an open script/text tab from `from` to `to` (it was editing a file that is not there any more; without this it would show a "file removed" state) and drop thumbnails and the inspector's `GetAssetInfo` cache for `previousUuid`. Anything the frontend holds by asset UUID must use `previousUuid -> uuid`.

## 5. Frontend UI (outline)

- Asset rows (files and folders) become drag sources; folder rows and the breadcrumb/root row become drop targets. Dragging a row that is part of the selection drags the whole selection (the panel already has Ctrl/Shift multi-select); dragging an unselected row drags just that row.
- Drop on a folder row builds `moves = selection.map(p => {from: p, to: folder + "/" + basename(p)})`, dropping entries already in that folder and entries that are a descendant of another selected entry (the host would refuse both). Drop on the root row targets `res://`.
- Drop feedback: a valid target is highlighted; an invalid one (a selected folder itself or one of its descendants, the folder an item is already in, a destination with a case-insensitive name collision found from the already-listed folder contents, the folder holding the open scene) shows a not-allowed cursor and no highlight. The host remains the authority and its Error is shown in the panel's existing error text.
- A drop that comes back with `warnings` shows them in a dismissible notice ("res://scripts/a.lua still names scripts.util"); the move already happened.
- Keyboard: Cut and Paste of rows may be added as the same command (a later step); a rename (F2) can reuse the same command with a changed name.
- Disabled whenever the session is a viewer, the host is read-only (both known from `Hello`), or the host is a play host.
- A confirmation dialog is not needed (the move is reversible and the redirect keeps references), but moving a folder of more than N assets (suggest 200) asks once.

## 6. Test plan

Engine (`tests/engine`, the `ResourceLoader` tests): the redirect: move a file and `LoadByUUID(oldUuid)` / `GetMetadata(oldUuid)` return the asset; the old res:// path resolves; a sub-asset's old UUID resolves; a live file created at an alias path drops the alias with a warning; chain A -> B -> C keeps both old UUIDs; the round trip B -> A restores the original UUID and an empty `movedFrom`; the new .meta has the right `resourcePath`, `uuid`, `customData` (import settings kept) and a rescan after a fresh `Initialize` rebuilds the aliases from `movedFrom` alone; `ResourceLoader::MoveAsset` for a folder re-keys every asset below it; read-only loader writes no .meta; a stale .meta at the destination is replaced.

Editor host (`tests/editor-server/project/EditorAssetTests.cpp`, new `EditorAssetMoveTests.cpp` in the same fixture): a move answers `AssetsMoved` with the pairs and `previousUuid`; a scene that references a moved texture/script by the old UUID loads with the reference intact (the headline test) and saves with the new UUID; `project.n2proj` `startupScene` and `scenes` are rewritten when the startup scene moves; refusals, each leaving the disk and the index unchanged: collision (including case-insensitive), folder into itself and into a descendant, overlapping sources, the open scene's file and a folder holding it, a missing source, a bad path, a non-asset file, a `.meta`, a changed extension, a symlink, over the count caps; atomicity: inject a failure (a rename hook or a read-only target folder on Linux CI) into the second of three moves and assert the first is rolled back and one Error names the second; a loaded `LuaScript` and its `LuaComponent` keep running and save the new UUID after the move (`WritingALoadedScriptReloadsItInPlace` at `EditorAssetTests.cpp:861` is the model); the `assetsChanged` event has `moved` and the watcher sends no second event (`:1093`); roles and read-only: viewer forbidden, read-only host `readOnly` error, play host refused (`EditorExtensionPlayTests`, `PollEventsRoleTests` patterns); warnings for a `.lua` that requires the old module name; case-only rename (platform guarded). `EditorHistoryTests`: undo of a scene edit made before a move still restores references (old UUIDs resolve).

Protocol: add `MoveAsset` and `AssetsMoved` to `editor-server/protocol/protocol.json` (beside `CreateFolder`, `protocol.json:579`), bump `version` to 1.24.0 and `Protocol.hpp:14`, extend the `PollEvents` description with the `moved` key; run all four generators (`editor-server/protocol/generators/generate_cpp.py`, `generate_csharp.py`, `generate_typescript.py`, `generate_test_vectors.py`) and commit `editor-server/clients/**` and `test-vectors.json`: the CI `protocol` job (`.github/workflows/tests.yml:67-96`) regenerates them and fails on any diff, then runs `editor-server/protocol/tests/run_tests.py`. `tests/editor-server/GeneratedClientCheck.cpp` and the `ProtocolVersion` consistency test need the new version.

End to end: `tests/smoke/editor_host_smoke.py` gets a scenario step in the `host` scenario: create a folder and a texture-bearing project, `MoveAsset`, then `ListAssets`, `GetAssetInfo` by the old UUID, `OpenScene` of a scene that references it, and a restart of the host to prove the aliases come back from `.import`. The Linux and Windows legs run it.

Docs (same PR as the code, per the standing rule): `docs/editor.html` (remove the MoveAsset line at `:818`, add a section: command, redirect, edge cases), `docs/logging-and-editor.html` command table and the version line (`:365`), and the file-level `Source:` lines. `docs/mcp.html` is not touched by the engine PR (the bridge section follows the bridge PR).

## 7. Size, risk, order of PRs

| PR | Content | Size | Risk |
|---|---|---|---|
| 1. Engine | `AssetMetadata.movedFrom`, `ResourceLoader::MoveAsset` + alias maps + path redirect in `Resources`, `LuaComponent::RetargetScript`, engine tests | M (about 600 lines of code, 400 of tests) | Medium: the loader's maps are the heart of every asset lookup; the alias precedence rule is the sharp edge. Mitigated by a live-wins rule and by tests of the collision cases |
| 2. Host + protocol + docs | `MoveAsset` handler, validation, journal and rollback, `project.n2proj` rewrite, warnings scan, `assetsChanged.moved`, protocol 1.24.0, generated clients, vectors, smoke, docs | M-L (about 700 lines, 600 of tests) | Medium: file system rollback on Windows, case-only rename, the open-scene refusal. The CI protocol job must pass |
| 3. Frontend (frontend repo) | protocol sync to 1.24.0 (types, `moveAssets` client call), drag and drop in the Assets panel, refresh and tab retargeting, DOM tests | M | Low-medium: UI only, the host is the authority |
| 4. MCP bridge (frontend repo) | `n2_move_asset` write tool, tests, docs/mcp.html | S | Low |

Order: PR 1, then PR 2 (they could be one PR; split because the loader change is reviewable on its own and the engine tests run without the host), then PR 3, then PR 4. PRs 1 and 2 are what this repository ships; the frontend and bridge cannot start before the protocol PR is on master (the generated TypeScript client is the frontend's input). Total: about two weeks of agent time for 1+2, one for the frontend, a day for the bridge. Protocol minor 1.24.0 is the next free but is taken serially: check `master` when PR 2 is opened.

## DECISIONS for the user (each with my recommendation)

1. **How references survive a move.** Options: refuse, rewrite files, warn only, or redirect (the loader keeps old UUIDs and paths as aliases, `.meta` gets `movedFrom`). Recommend the redirect, plus an automatic rewrite of `project.n2proj` and a non-blocking warning for Lua `require` and path strings in text. Not "keep the UUID" (it needs collision handling that is the redirect again).
2. **Atomicity.** All-or-nothing: validate everything first, then roll back completed moves if one fails, one Error. Recommend this; the alternative (best effort, report per entry) leaves a half-moved project with no undo.
3. **Collisions.** Refuse, never overwrite, and compare case-insensitively on every platform. Recommend this.
4. **The open scene.** Refuse to move its file or a folder holding it (as `WriteTextAsset` does). Recommend refusing in v1; the alternative is to update `_openScenePath` and the scene UUID live, which needs more care for autosave and dirty state.
5. **Command shape.** `json moves` as a list of `{from, to}` (also renames), id 0xAF, response `AssetsMoved` (id 0xA5), protocol 1.24.0 (serial). Recommend this over "sources plus destination".
6. **Event shape.** `assetsChanged.moved` in addition to the moved paths in `added` and `removed`, so unchanged clients keep working. Recommend this.
7. **Script handling.** Add `LuaComponent::RetargetScript` so a running script keeps its instance state and saves the new UUID. Recommend this; the fallback is a reload of the scene.
8. **Bridge.** Add `n2_move_asset` as an ordinary (not irreversible-gated) write tool, after the frontend ships, capped at 50 moves. Recommend this.
9. **Moves into or out of `res://editor` (extensions) from the UI.** The host has no rule today. Recommend keeping parity in v1 and filing a separate item to decide it for all write commands.
