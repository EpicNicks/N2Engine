# Moving assets between folders from the editor: design proposal

Status: design, no code. Written against master at a0472d54, editor protocol 1.23.0; line numbers below are for that commit. Revised after the user's decisions: references survive by STABLE UUIDS (an asset keeps its UUID when it moves), not by redirect aliases; decisions 2-7 of the first version are accepted as written.

The Assets panel cannot drag a file or a folder onto another folder. New scripts, scenes and prefabs are created in the selected folder, but nothing moves an existing asset, and the protocol has no command for it.

## 0. The finding that shapes everything: today a move changes the UUID, so the UUID must stop being derived from the path

`docs/editor.html:818` says `MoveAsset` stays post-MVP because "a move changes the asset's UUID, which needs a rewrite of every reference". The code confirms it:

- An asset's UUID is derived from its res:// path. `ResourceUUID::FromPath` is `GenerateNameBased(projectNamespace, path.ToString())` (`engine/src/io/ResourceUUID.cpp:56-65`); a sub-asset's is `FromSubAsset(parentPath, key)` (`ResourceUUID.cpp:67-77`).
- The loader enforces it. When a .meta is read and its `uuid` differs from `FromPath(path)`, the loader logs "UUID mismatch ... Regenerating" and overwrites the .meta's uuid (`engine/src/io/ResourceLoader.cpp:329-337`). A new file gets `FromPath` too (`ResourceLoader.cpp:363`).
- The .meta is not beside the file: it lives in `<project>/.import/<same relative folder>/<name>.meta` (`ResourceLoader.cpp:107`, `GetMetadataPath` at `382-389`) and is committed (`.n2/` is the ignored per-machine folder: the generated `.gitignore` holds only `.n2/`, `ProjectFile.cpp:617`). It stores `uuid`, `resourcePath`, `resourceType` and `customData` (import settings, the model sub-asset index) (`engine/src/io/AssetMetadata.cpp:22-30`, `SaveToFile` just below).
- The scan keys `_metadata` by `meta.resourcePath` (`ResourceLoader.cpp:262-273`), and in the existing-.meta branch (`:323-360`) that value is whatever the .meta file says: it is never re-derived from the file's real path. A .meta copied by hand to another `.import` folder is indexed under its OLD path today. This is fixed below (3.1).
- A rename by the file system is seen as delete plus add: the old path is dropped from `_metadata`, `_uuidToPath` and the caches but "Their .meta files stay" (`ResourceLoader.cpp:166-199`); the file at the new path gets a brand new .meta and UUID, and import settings are lost.

**Decision 1 (user, revised): stable UUIDs.** The UUID is stored in the asset's .meta and that is its identity. A move carries the .meta with it (rewriting its `resourcePath`), the UUID does not change, and every reference by UUID keeps working with no alias machinery: scenes, prefabs, materials, `LuaComponent` `scriptUUID`, the planned prefab fields, undo history, autosaves. The first version of this document recommended redirect aliases and listed "make the UUID stable" as worse (collisions, sub-assets, determinism); section 3 answers each of those objections concretely, and section 3.7 lists what the user takes on with this choice.

## 1. Command shape

### 1.1 MoveAsset

- **Id 0xAF** (the next free id in the 0xA0 block; `Protocol.hpp:95-109` uses 0xA0-0xAE) and a new response type (`AssetsMoved`, id 0xA5, the next free; response ids in use are 0xA0-0xA4, `Protocol.hpp:185-191`).
- **Request**: `json moves`, an array of `{from, to}` res:// paths, 1..256 entries. `to` is the new full path of the asset (not the destination folder), so the same command also renames. The frontend computes `to = destinationFolder + "/" + basename(from)` for a drop.
- **Files and folders**: `from` may be a file with a loader (an indexed asset) or a folder. A folder moves as one directory rename plus a rewrite of every indexed .meta under it. Files with no loader (a `.txt` in assets) move with their folder; moving a single unindexed file is refused (`Not an asset`), as `SetImportSettings` does (`EditorServer.cpp:7923`).
- **Response** `AssetsMoved`: one JSON field `result = {moved: [{from, to, uuid, type}], warnings: [string]}`. `uuid` is the asset's UUID, the same before and after (there is no `previousUuid`). For a folder, `moved` lists the folder entry (no uuid) and each indexed asset under it (capped at 10000 assets per call, else Error before anything is touched).
- **Since**: 1.24.0. It is the next free minor, but minors are taken serially: whichever protocol PR merges first takes 1.24.0 and the next one rebases onto 1.25.0. Check `Protocol.hpp:14` and `protocol.json:2` at merge time.
- **Class, roles, read-only, play host**: register `{CommandType::MoveAsset, "MoveAsset", CommandClass::Write}` beside `CreateFolder` (`EditorServer.cpp:4276`) and add it to `IsEditOnlyCommand` (`EditorServer.cpp:5975`). Nothing else is needed:
  - read-only host: `IsReadCommand` is an allow list, so a command not classified as a read is refused with the readOnly error before the handler runs (`EditorServer.cpp:4421-4427`);
  - roles: a viewer is refused (Write class), editor and admin are allowed (`RoleAllows`, `EditorServer.cpp:2324-2341`), answered with ErrorCoded forbidden;
  - play host: refused by `IsEditOnlyCommand` (`EditorServer.cpp:4442-4446`).
- **Audit line**: the generic `audit: user=... role=... command=MoveAsset` line comes for free (`EditorServer.cpp:4451-4459`), and it names no path (as for CreateFolder). Add one `ServerLog::Info` per call, "Moved N assets (first: res://a -> res://b)", with the paths passed through `SanitizeForLog`, as `HandleCreateFolder` does ("Created folder", `EditorServer.cpp:8151`).
- **Not polled**: it is a write, it logs.
- **MCP bridge**: `n2_move_asset` lives in the frontend repository and is NOT part of this repository's work; it comes last (section 8). Notes for whoever writes it: an ordinary write tool (not irreversible: a move is undone by moving back, and with stable UUIDs the round trip changes nothing a scene can see), cap of 50 moves per agent call, keep the bridge's path policy (plain `res://`, no `..`, first segment not `editor`, never `.meta`; `docs/mcp.html:174`), surface `warnings` to the model as data.

## 2. What a move keeps, and what it still breaks

### 2.1 By UUID: all of it survives, nothing to rewrite

| Place | Evidence | After a move |
|---|---|---|
| Component asset references in .scene and .prefab JSON | saved as `assetRef->GetUUID().ToString()`, resolved by `ResolveAssetReference` -> `Resources::LoadByUUID` (`ComponentSerializer.hpp:360-395`, `:403-410`) | Resolve: the UUID is the same |
| LuaComponent script | `j["scriptUUID"] = loader.GetUUID(_scriptPath)` (`LuaComponent.cpp:1375-1387`); load: UUID -> `GetMetadata(uuid)` (`:1405-1421`) | Resolves. The loaded component's `_scriptPath` still names the old path until `RetargetScript` (3.5 below) |
| Material (.mat) textures given as UUID | `Material.cpp:127-137` | Resolve |
| Model sub-asset UUIDs | `FromSubAsset(parentPath, key)` today (`ResourceLoader.cpp:688, 739, 1038`) | Stay, by the seed rule in 3.2 |
| Scene's own UUID | `FromPath(file)` at `EditorServer.cpp:9291, 9347, 9404`, `n2server/src/SceneLoading.cpp:82` | Changed to the asset's .meta UUID (3.6) so a moved scene keeps its UUID |
| Edit history and `.n2/autosave/` | scene JSON snapshots with UUIDs (`docs/editor.html:439`) | Restore correctly |

### 2.2 By PATH: still broken by a move, handled by the host or warned

| Place | Evidence | Handling |
|---|---|---|
| `project.n2proj` `startupScene` and `scenes` | res:// paths (`ProjectFile.hpp:24, 45-46`; `ProjectFile.cpp:223-229, 316`; `SetStartupScene` normalises and saves, `EditorServer.cpp:9839-9867`) | The host rewrites them in the same call through `SaveProject` (the path `SetStartupScene` uses, `:9862`), so the file is right for tools that read it without the engine (n2server, the CI smoke) |
| Lua `require("scripts.util")` | module name is the path with dots (`LuaRuntime.cpp:651-668`, `PathToModuleName` `~685`) | Warned, not blocked: the host scans `.lua` files (at most 2000 files of at most `MaxTextAssetBytes`) for the old module name in quotes and for the literal old `res://` path, and reports `warnings` such as `res://scripts/a.lua still names scripts.util` |
| `res://...` strings in script text, `scriptData` strings, `res://models/x.glb#mesh/Body` sub-asset references (`LuaComponentBindings.cpp:175-200`), a .mat texture given as a path (`Material.cpp:127-137`), the `scriptPath` fallback of old scenes (`LuaComponent.cpp:1424-1432`) | | Not rewritten (no aliases any more): the text scan above warns for what it can see; the rest is unknowable. These break the way they would after any rename. A later optional `rewriteReferences` flag could rewrite scene/prefab/material path strings; not v1 |
| `LuaRuntime::ReloadModule` and the declaration cache, keyed by module name / path (`LuaRuntime.cpp:787-830`) | | Updated by `RetargetScript` (3.5) |
| Frontend state keyed by path: open script tabs, selection, expanded folders | | Remapped from `from` to `to` on the response/event (section 5). State keyed by UUID (thumbnails, `GetAssetInfo` cache) needs nothing |
| Open scene path, dirty state | `_openScenePath` (`EditorServer.cpp:9165-9169, 9217, 9407`) | Refused in v1 (4) |

### 2.3 Why not aliases

Rejected by the user. For the record: aliases needed a second UUID map, `movedFrom` bookkeeping with a live-wins rule, and left old UUIDs in saved files until each scene was re-saved. Stable UUIDs have one rule (the .meta says), at the price of the cases in 3.

### 2.4 Engine API for the host

A new `ResourceLoader::MoveAsset(const ResourcePath &from, const ResourcePath &to)` (and a folder variant that takes the indexed assets under a prefix) that: writes the new .meta (new `resourcePath`, SAME `uuid`, same `resourceType` and `customData`, `uuidSeed` set as in 3.2); re-keys `_metadata`, `_uuidToPath` (path value only), `_assetState`, `_subAssets` (parent path) and the caches by path. The cached asset keeps its UUID and so its `_cacheByUUID` entry. This is the single place that owns the .meta, as `SetImportSettings` and `SetSubAssetIndex` are today (`ResourceLoader.hpp:131-166`).

## 3. The UUID model

### 3.1 The rule: the .meta is the source of truth

Today `FromPath` is the identity and the .meta a cache of it. After this change the .meta is the identity and `FromPath` only the way to MINT one. Each case, with the code that changes (all in `ResourceLoader::CreateOrUpdateMetadata`, `ResourceLoader.cpp:292-378`, and the scan that calls it, `:228-280`):

1. **A new asset** (no .meta, `:361-372`): `uuid = FromPath(creationPath)`, the same value as today, then stored in the new .meta (as `PersistMeta` already does). **Why this is safe for every existing project:** the loader enforces `uuid == FromPath(path)` on every scan today (`:329-337`), so in every project that has ever been opened, every .meta already holds exactly the path hash of its current path. "Keep what the .meta says" therefore changes nothing for them: no .meta is rewritten, no UUID changes, nothing migrates. A clone without `.import` (never opened) still computes the same UUIDs as before, as long as nothing in it has moved. The projectId namespace (`ResourceUUID.cpp:48-54`, `docs/editor.html:148`) is still the namespace of minted UUIDs.
   - If the minted UUID is already held by another indexed asset (the case that stable UUIDs create: `a.png` moved to `b/a.png` keeps `FromPath(res://a.png)`, then a new file is created at `res://a.png`), the new asset gets `GenerateNameBased(namespace, "res://a.png#2")`, then `#3`, until free. Deterministic, so two clones that scan the same files agree.
2. **A .meta whose `uuid` differs from `FromPath(path)`** (today regenerated with a warning, `:329-337`): now KEPT, with no warning (it is the normal state of a moved asset; one `Info` line at most). Two exceptions that are still regenerated, with a warning, because they are broken rather than moved: a `uuid` that is all zeros (written when `ResourceUUID` was not initialised, `docs/resources.html:188`, where "every asset gets the same UUID"), and a duplicate (3.4). One side effect worth stating: changing a project's `projectId` (or adopting an old one with `--project-id from-path`, `Host.cpp:121, 259-261`) used to regenerate every .meta into the new namespace and break scene references; now the .metas keep their UUIDs, so references survive it.
3. **The .meta's `resourcePath` is wrong or stale**: the file at hand decides. In the existing-.meta branch, `meta.resourcePath` is set from the file's real path, and the .meta is saved if it differed. This fixes the hand-copied-.meta indexing bug of section 0 and is also what makes an out-of-editor move of file AND .meta work (3.5).
4. **A .meta missing or lost** (deleted, or never committed: `.import` in a `.gitignore`, a corrupt file, `:308-312`): treated as a new asset, `FromPath(current path)`. For an asset that never moved this is exactly its old UUID, so nothing breaks; for an asset that HAD moved, its UUID is gone and every reference to the old one breaks, silently except for the existing `Asset ... not found` log lines. A corrupt .meta logs a warning (as today, `:311`), which now also says "references to its old UUID will not resolve". A missing .meta cannot be told from a new asset and logs "New asset" as today.
5. **Two files, one UUID**: 3.4.
6. **A file moved outside the editor** (the OS, git): 3.5.

Read-only loaders (the play host, `ResourceLoader.hpp:49-62`) apply the same rules in memory and write nothing (`PersistMeta` is already skipped when read-only).

### 3.2 Sub-assets: a stored seed path

A model's sub-asset UUID is `FromSubAsset(parentPath, key)`, hashed from the parent's PATH. If the parent moves and the loader keeps hashing the new path, every mesh, material and texture part gets a new UUID and scenes lose them. The sub-asset index stores `{key: {type, uuid}}` (`ResourceLoader.hpp:158-166`) but the scan deliberately re-derives rather than trusts it (`:739`, `:1038`: "a moved file's index names its old path's UUIDs").

Rule: `AssetMetadata` gets one optional top-level `.meta` key, `uuidSeed`, a res:// path string: "the path the sub-asset UUIDs of this file are hashed from". Absent means the file's own `resourcePath`, which is the case for every existing .meta, so nothing changes for them. `MoveAsset` writes `uuidSeed = previous uuidSeed, or the old path if absent` and never overwrites it afterwards. The three call sites (`:688` RegisterSubAssets, `:739` ReadSubAssetIndex, `:1038` GetSubAssets) hash `FromSubAsset(seed(meta), key)` instead of `FromSubAsset(parentPath, key)`. A lost .meta (3.1.4) gets seed = its current path, as above. A move back to the original path leaves the seed equal to the path, harmless.

(Alternative considered: seed = the stable file UUID instead of a path. Rejected: it would change every existing sub-asset UUID and break every existing scene that references a model part.)

### 3.3 Why not store a separate "identity" UUID on top of the path hash

Not needed: the .meta `uuid` already IS that field and is already written (`AssetMetadata.cpp:22-30`); only the loader's overwrite rule had to go.

### 3.4 Two files with the same UUID

Causes: a file copied in the explorer together with its .meta (or `.import` copied along with `assets`), a restored backup beside the live project, a merge that brings both. Detection at index time: `ScanDirectory` records `_uuidToPath[meta.uuid] = path` (`:273`) and `RefreshAsset` does the same (`:828`), silently overwriting an earlier holder today. New: after the scan (and in `RefreshAsset`), if a UUID would be held by two different live paths, one KEEPS it and the other is regenerated (`FromPath` of its own path, then the `#2` rule of 3.1.1), its .meta rewritten, and one warning logged naming both ("res://b/copy.png had the same UUID as res://a.png: gave it a new one; references to the UUID go to res://a.png"). Which keeps it is deterministic: the one that held it in the index before this scan; if none (the first scan of a project), the lexicographically smallest res:// path (ASCII case-folded). Not directory order, which differs per file system and would make two machines disagree. In a read-only loader the regeneration is in memory only.

### 3.5 A move outside the editor, and the scene-in-flight cases

A file moved by the OS or git is delete + add to the watcher (`ResourceLoader.cpp:166-199`) and, unless the .meta was moved too, the new path gets a new .meta and a new UUID (`FromPath(newPath)`, or the `#2` rule); references to the old UUID break. v1 does not guess (no content-hash pairing). If the person also moves the .meta under `.import/` (git mv of both), 3.1.3 makes it work: the UUID is kept and `resourcePath` is rewritten. The old orphan .meta of the first case stays on disk, by design.

### 3.6 Scene UUID

Scenes are assets with a .meta. Their UUID is set from `FromPath(file)` at `EditorServer.cpp:9291, 9347, 9404` and `n2server/src/SceneLoading.cpp:82`; those four sites change to the loader's UUID (`loader.GetUUID(path)`, falling back to `FromPath` when the loader says ZERO, as `EditorServer.cpp:8114, 8254` already do). For every unmoved scene the value is identical to today's, so the tests that compare with `FromPath` (`EditorProjectTests.cpp:227, 298, 423, 469, 502`) still hold.

### 3.7 Every use of FromPath, and what it means outside the editor

All production uses (grep of `FromPath`/`FromSubAsset`, excluding `ResourcePath::FromPath`):

| Use | Where | Effect of stable UUIDs |
|---|---|---|
| Minting, existing-meta check, new meta | `ResourceLoader.cpp:330, 363` | The change itself (3.1) |
| Sub-asset UUIDs | `ResourceLoader.cpp:688, 739, 1038`, `ResourceUUID.cpp:67` | Seed rule (3.2) |
| Scene UUIDs | `EditorServer.cpp:9291, 9347, 9404`, `SceneLoading.cpp:82` | Loader UUID (3.6) |
| Fallback when the loader answers ZERO | `EditorServer.cpp:8114, 8254` | Unchanged |

There is NO runtime path-to-UUID resolution that bypasses the loader: scenes carry UUIDs and `LoadByUUID` goes through `_uuidToPath`; `LuaComponent` and `ComponentSerializer` read `loader.GetUUID(path)` (metadata lookup). Network replication keys are game object UUIDs, which live in scene files, not asset UUIDs (`ReplicationLayout.cpp:188, 203`). The tests that assert `meta->uuid == FromPath(path)` (`ResourceLoaderRescanTests.cpp:192, 215`, `ResourceLoaderAssetStateTests.cpp:278`, `ResourceLoaderReadOnlyTests.cpp:83`, `MaterialAssetTests.cpp:481, 493`, `EditorAssetTests.cpp:328, 718`) are about never-moved assets and stay true; no golden or determinism test hashes an asset UUID (the software-renderer goldens are pixels; the protocol vectors carry no asset UUIDs).

What matters is where a runtime has NO `.import`:

- **Every runtime builds its own index from the project folder**: `lua_project/src/Main.cpp:122-132`, `n2server/src/Server.cpp:463`, the editor host, and the web player (`web/player/WebPlayer.cpp:452-462`) all call `ResourceUUID::Initialize` then `ResourceLoader::Initialize(projectDir)`. With a `.import` beside `assets/` they now read the stable UUIDs; without it they mint `FromPath`, which equals the stable one for every asset that never moved.
- **The web package does not carry `.import`**: `web/CMakeLists.txt:54-55` preloads only `assets/` and `project.n2proj`. A web build of a project with a moved asset would silently mint the path hash and lose every reference to it. The code PR adds `.import` to the preload when the folder exists (and `docs/web-build.html:206` says so). The web CI job runs the default `lua_project`, which has no `.import` and no moves, so it does not exercise this; it is verified by a unit test of the loader with the .meta present and by a configure-time check, not by running a browser.
- **A deployment that ships `assets/` without `.import`** (a server copy, a CI cook) has the same problem; the docs state that `.import` is now part of the project's identity and must be shipped and committed.
- **A project that gitignores `.import`** (the generated `.gitignore` does not, `ProjectFile.cpp:617`) loses moved assets' UUIDs on every fresh clone.

### 3.8 Compatibility story

- **Existing projects, new engine**: nothing migrates and nothing is rewritten (3.1.1). The first move is what first creates a UUID that differs from the path hash.
- **`.import` stops being disposable.** `docs/editor.html:148` promises "`.import/` can be deleted and regenerated with the same UUIDs". After this change that holds only for assets that never moved. The docs say it plainly: commit `.import`, ship it with exported builds.
- **Old engine, new project (the real risk)**: an engine without this change, opening a project in which something has moved, sees the .meta `uuid` differ from `FromPath(path)` and regenerates it (`:329-337`), silently undoing the move's stability and breaking scenes that saved the stable UUID; it also ignores and drops `uuidSeed` when it rewrites the .meta. There is no way to make an old engine honour the new rule. Mitigation: the protocol bump to 1.24.0 tells a client when the host has the feature; the docs call out that a team must update engines together before moving anything; a move on a project is never forced on anyone.
- **New project, old clients**: the `assetsChanged` `moved` key and the `moved` response are additive (section 4).
- **The mismatch warning is gone**, so a hand-edited .meta uuid is trusted; the duplicate check (3.4) is the only guard.

## 4. Edge cases

**Order of operations per entry, crash safe** (no undo exists for file operations):
1. Validate everything first, touching nothing (below). Any failure: one Error, nothing changed.
2. Write the NEW .meta atomically (`WriteTextFileAtomically`, as `SaveToFile` does): new `resourcePath`, same `uuid`.
3. `rename` the file (or the folder).
4. Remove the OLD .meta (or the old `.import` subtree).
A crash after 2 leaves an extra .meta with the same UUID as the live one: the next scan finds the duplicate (3.4, it is a stale .meta with no file, so it is not indexed at all, harmless; stale .metas are kept by design, `ResourceLoader.cpp:167`). A crash after 3 leaves the file moved, the new .meta valid, and an old .meta that is stale. At no point does a file sit at a path with a .meta that has lost its import settings or its UUID.

**Partial failure and atomicity** (decision 2, accepted). All-or-nothing in the validation phase (every check below runs for every entry first). In the execution phase the host keeps a journal of completed steps; if entry k fails (the file is open by another process on Windows, a permission error, a full disk), it undoes entries k-1..1 in reverse (rename back, delete the new .meta) and answers a single Error naming the entry that failed and the reason (through `ClientText::Failure`, as `HandleWriteTextAsset` does, `EditorServer.cpp:8054-8057`). If a rollback step itself fails, the Error says so and lists the entries left half-moved, and the host logs it as an error; the next watcher rescan then reconciles the index with the disk. With stable UUIDs a half-moved entry (file moved, .meta not yet) is the out-of-editor case of 3.5 only if its .meta write was lost; the order above writes the new .meta first, so it is not. No success is reported for a call that rolled back, and no event is sent for it.

**Validation (all before any change):**
- Paths: `ResolveAssetPath` on `from` (must exist) and `to` (`EditorServer.cpp:7517-7591`): res:// only, valid names on every platform (`IsValidAssetName`: no `<>:"|?*\`, no trailing dot/space, no Windows device names, at most 255 bytes), inside the assets folder, symlinks cannot lead out. A source that is itself a symlink: refused (as `ListAssets` skips links, `EditorServer.cpp:7771`).
- `to` is not the assets root. The parent of `to` must exist, or the call creates it (recommended: create, like `CreateScriptAsset`, `:8097`).
- A file keeps its extension (a move that changes the extension is refused).
- **Name collision at the destination** (decision 3, accepted): refuse, never overwrite. Compared case-INSENSITIVELY on every platform. A stale .meta (no file) at the destination is replaced by the moved asset's.
- **Folder into itself or a descendant:** refuse when `to == from` or `to` starts with `from + "/"`, on resolved, case-folded paths. Also refuse a list in which one source is inside another, or two entries have the same `to`.
- **Same place:** `from == to` is an Error ("already there").
- **The open scene** (decision 4, accepted): refuse a move of the open scene's file, or of a folder that contains it, as `WriteTextAsset` refuses to write it (`EditorServer.cpp:8038-8042`, `OpenScenePath()` at `:9165`). The message says "save the scene elsewhere with SaveSceneToFile first".
- **Moves under `res://editor`** (decision 9): the host has no special rule for the extension folder today; v1 keeps parity, and a separate item decides it for every write command.
- **Windows path length:** refuse when the destination's absolute path would pass 240 characters on Windows.
- **Counts:** at most 256 entries and 10000 indexed assets per call.

**A script currently loaded in memory** (decision, accepted: `LuaComponent::RetargetScript`). The watcher's reload works by path: `ReloadLoadedAsset` sets the new source into the cached `LuaScript` and calls `LuaRuntime::ReloadModule(path, script)` (`EditorServer.cpp:7593-7616`). After a move every `LuaComponent` would keep `_scriptPath` = the old path; its next save writes `scriptPath` = old with no UUID (the metadata lookup of a path that is not indexed is ZERO, `LuaComponent.cpp:1375-1387`) and its diagnostics name the old file. So the host, after the loader has moved the asset, re-points the open scene's LuaComponents whose script is under the moved set to the new path with `LuaComponent::RetargetScript(newPath)`: it changes `_scriptPath` and the module bookkeeping (erase the old module name in `_loadedModules`, forget the old declaration-cache entry, `ReloadModule` the new path) and does not run `OnDestroy`/`OnCreate` or lose `_scriptData`. The script's UUID is the same, so the saved `scriptUUID` is right either way.

**The watcher seeing delete + add.** The host does the whole change itself and then calls the loader's refresh for what it touched, exactly as `NoteAssetWritten` does after a write (`EditorServer.cpp:7662-7683`): `_metadata` is already updated when the watcher's next `AssetsChangedOnDisk` fires (`:7694`), so the rescan finds nothing new for those paths and sends no duplicate event (model test: `TheWatcherDoesNotRepeatWhatACommandAlreadyReported`, `EditorAssetTests.cpp:1093`).

**Case-only renames** (`Texture.PNG` -> `texture.png`, a folder `Props` -> `props`). On Windows and default macOS `exists(to)` is true for the same file, and `ResolveAssetPath` canonicalises an existing target to the spelling the file system has (`EditorServer.cpp:7576-7588`), which would silently turn the new spelling back into the old one. The handler resolves `to` lexically when `std::filesystem::equivalent(from, to)`, treats it as a case-only rename and performs it as two renames through a temporary name in the same folder (journal both steps). On Linux it is an ordinary rename. The UUID does not change.

**Windows versus Linux.** A folder rename fails on Windows if any file below it is open in another process; that is the rollback case above. The host holds no handle to an asset file between commands. Rename never crosses volumes here; if `.import` is a mount point the rename fails and rolls back, reported as such.

**Import settings.** They travel with the .meta. The old .meta at the source is removed (step 4) so a later file put at the old path starts clean and gets a fresh UUID (3.1.1, including the `#2` rule when the old path hash is still held by the moved asset).

## 5. Events and the frontend refresh

The host sends the existing `assetsChanged`, with one new key:

```json
{"added": ["res://textures/hero.png"],
 "removed": ["res://hero.png"],
 "modified": [],
 "moved": [{"from": "res://hero.png", "to": "res://textures/hero.png", "uuid": "<same before and after>"}]}
```

- A moved asset is listed in `moved` AND in `removed` (its old path) and `added` (its new path). A client that ignores `moved` (any 1.23 client) refreshes correctly: it sees a file leave and a file arrive. The event is JSON and "a client ignores kinds and keys it doesn't know" (`protocol.json` PollEvents description).
- A folder move lists the folder's indexed assets individually, capped like the hook lists (`ExtensionHost::MaxHookAssetPaths = 1000`, `ExtensionHost.hpp:183`); beyond the cap the event carries the capped list and clients refetch with `ListAssets` anyway.
- The response `AssetsMoved` carries the same pairs, so the issuing client can update without waiting for the event.
- `OnAssetsChanged` hooks (`EditorServer.cpp:7644-7659`) keep receiving added/removed/modified and get `moved` as a fourth, capped list.
- The ListAssets folder list comes from the file system (`EditorServer.cpp:7761-7787`); an empty-folder move changes no indexed asset, so the event can be empty; the response and the frontend's refetch cover that.
- **Frontend refresh** (outline; separate repository): refetch the folders it shows, remap the selection and expanded-folder state from old to new paths, retarget an open script/text tab from `from` to `to`. Anything the frontend holds by UUID (thumbnails, inspector cache) is still valid: no `previousUuid` remap exists.

## 6. Frontend UI (outline)

- Asset rows become drag sources; folder rows and the root row become drop targets. Dragging a selected row drags the selection; an unselected row drags just that row.
- Drop on a folder row builds `moves = selection.map(p => {from: p, to: folder + "/" + basename(p)})`, dropping entries already in that folder and entries that are a descendant of another selected entry.
- Drop feedback: invalid targets (itself or a descendant, the folder it is in, a case-insensitive name collision, the folder holding the open scene) show not-allowed; the host remains the authority and its Error is shown in the panel.
- A drop with `warnings` shows them in a dismissible notice; the move already happened.
- Disabled for a viewer, a read-only host, or a play host (all known from `Hello`).
- Moving a folder of more than 200 assets asks once.

## 7. Test plan

Engine (`tests/engine/io`, the `ResourceLoader` tests): a new asset's UUID is `FromPath` of its creation path and is written to the .meta; a .meta whose uuid differs from the path hash is kept (no rewrite, no warning); a zero uuid is regenerated; a stale `resourcePath` in the .meta is corrected from the file's real path; a missing .meta re-derives the path hash and a corrupt one warns; two files with one UUID: the first keeps it (deterministic order), the other is regenerated with a warning; a new file at the old path of a moved asset gets the `#2` UUID; a read-only loader writes nothing in any of these; `ResourceLoader::MoveAsset`: file and folder, UUID unchanged, `.meta` `resourcePath` and `uuidSeed` right, `customData` kept, sub-asset UUIDs unchanged after the move and after a fresh `Initialize` (the model tests, `ModelAssetTests`), the cache entry survives; a stale .meta at the destination is replaced; a chain A -> B -> C; a move back; changing the projectId keeps stored UUIDs.

Editor host (`tests/editor-server/project/EditorAssetTests.cpp`, new `EditorAssetMoveTests.cpp` in the same fixture): a move answers `AssetsMoved` with the pairs and the unchanged `uuid`; a scene that references a moved texture/script by UUID loads with the reference intact (the headline test) and saves the same UUID; a moved scene (not the open one) opens with its old UUID; `project.n2proj` `startupScene` and `scenes` are rewritten; refusals, each leaving disk and index unchanged: collision (including case-insensitive), folder into itself and a descendant, overlapping sources, the open scene and a folder holding it, a missing source, a bad path, a non-asset file, a `.meta`, a changed extension, a symlink, over the count caps; atomicity: inject a failure into the second of three moves and assert the first is rolled back and one Error names the second; a loaded `LuaScript` and its `LuaComponent` keep running and keep their `_scriptData` after `RetargetScript` (`WritingALoadedScriptReloadsItInPlace`, `EditorAssetTests.cpp:861`, is the model); the `assetsChanged` event has `moved` and the watcher sends no second event (`:1093`); roles and read-only (viewer forbidden, read-only host `readOnly` error, play host refused); warnings for a `.lua` that requires the old module name; case-only rename (platform guarded). `EditorHistoryTests`: undo of a scene edit made before a move still restores references.

Protocol: add `MoveAsset` and `AssetsMoved` to `editor-server/protocol/protocol.json` (beside `CreateFolder`, `protocol.json:579`), bump `version` to 1.24.0 and `Protocol.hpp:14`, extend the `PollEvents` description with `moved`; run the four generators (`editor-server/protocol/generators/generate_cpp.py`, `generate_csharp.py`, `generate_typescript.py`, `generate_test_vectors.py`) and commit `editor-server/clients/**` and `test-vectors.json`: the CI `protocol` job (`.github/workflows/tests.yml:67-96`) regenerates them and fails on any diff, then runs `editor-server/protocol/tests/run_tests.py`. `tests/editor-server/GeneratedClientCheck.cpp` and the `ProtocolVersion` consistency test need the new version.

End to end: `tests/smoke/editor_host_smoke.py` gets a step in the `host` scenario: create a folder, `MoveAsset` a texture, `ListAssets`, `GetAssetInfo` by the UUID (same before and after), `OpenScene` of a scene that references it, and a host restart proving the UUID comes back from `.import`.

Docs (same PR as the code): `docs/editor.html` (remove the MoveAsset line at `:818`, add a section: command, stable UUIDs, edge cases; fix the design callout at `:148`), `docs/resources.html` (`:188`, `:257`, `:382`: UUIDs are stored, sub-asset seed), `docs/logging-and-editor.html` command table and version line (`:365`), `docs/web-build.html:206` (`.import` is packed), `docs/serialization.html` where it states UUIDs follow paths, and the file-level `Source:` lines. `docs/mcp.html` is not touched.

## 8. Size, risk, order of PRs

| PR | Content | Size | Risk |
|---|---|---|---|
| This one | Docs only: this design | - | - |
| Engine + host (one code PR, branch `feat-asset-move`) | the stable-UUID loader change, `uuidSeed`, duplicate detection, `ResourceLoader::MoveAsset`, `LuaComponent::RetargetScript`, `MoveAsset` handler, validation, journal and rollback, `project.n2proj` rewrite, warnings scan, `assetsChanged.moved`, protocol 1.24.0, clients, vectors, tests, smoke, docs, web package `.import` | L | Medium: the loader's UUID rule is the heart of every asset lookup (mitigated by "nothing changes for an unmoved asset" tests) and the old-engine hazard of 3.8 |
| Frontend (frontend repo) | protocol sync to 1.24.0, drag and drop, refresh, tab retargeting | M | Low-medium |
| MCP bridge (frontend repo) | `n2_move_asset`, last | S | Low |

## Decisions

Accepted by the user (2026-10-31): 1 stable UUIDs (replaces the redirect recommendation); 2 all-or-nothing with rollback; 3 name collisions refused, compared case-insensitively on every platform; 4 the open scene, or a folder holding it, refused in v1; 5 `moves` list, 0xAF, `AssetsMoved`, protocol 1.24.0; 6 `assetsChanged.moved` beside added/removed; 7 `LuaComponent::RetargetScript`; 8 the bridge tool last, in the frontend repository, not part of the engine work; 9 `res://editor` parity, a separate item.

Open consequences of decision 1 for the user to know, not to decide now: `.import` becomes part of a project's identity (commit it, ship it; 3.7, 3.8), and an engine older than the code PR silently regenerates the UUID of a moved asset (3.8).
