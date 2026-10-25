# Electron editor UI roadmap (design proposal, 2026-10-20)

Tags: [V] verified by reading N2Engine-Electron-Frontend `master` at `c3768c0` (protocol 1.13.0, `engine-ref.txt` = `2c15f3f`) and N2Engine `master` at `942cc21`; [M] from memory, general knowledge or another proposal, not checked in this code; [E] estimate. `[FE]` marks a frontend path (relative to N2Engine-Electron-Frontend/), other paths are relative to N2Engine/. Design only: nothing was built, run or changed, and no repo was touched.

Implementation update (2026-10-23; the design observations below describe the original baseline):

- FE13a/b, FE14 and FE15 are implemented together in [frontend PR #57](https://github.com/EpicNicks/N2Engine-Electron-Frontend/pull/57). This includes overlays and orthographic/axis/2D/bookmark views, CodeMirror with pinned Lua API completion data, and bounded client statistics. The combined frontend suites pass, and Chrome smoke covers the viewport controls, narrow statistics layout, and CodeMirror editing/completion/search/read-only behavior. Live Electron/host feel remains a user check.
- EN1 implements protocol 1.17.0: Read-class editor wireframe options, compile-only Lua syntax checking, and CPU texture/audio previews. The optional import-settings schema command is deferred. Backend wireframe support still applies (WebGPU ignores it; software does not rasterize it; Vulkan requires the device feature).
- EN2 implements protocol 1.18.0: `SerializeEntities` (Read; topmost subtrees as an `n2engine.entities` v1 payload) and `InstantiateEntities` (Write; one undo step, fresh UUIDs, shared remap across roots, `keepExternalRefs`, Unity-style unique names, 16 MiB / 10000 object / 512 level caps, `res://` escapes refused). `PrefabManager::InstantiatePrefabs` is the shared multi-root primitive; `InstantiatePrefab` (EN3) can wrap it. FE-S2 syncs next.
- EN3 implements protocol 1.19.0: `.prefab` assets (`PrefabFile` loader, resource type `Prefab`, not writable through `WriteTextAsset`) and four Write commands, ids `0xA9`-`0xAC`: `CreatePrefab(entityId, path, overwrite)` -> `AssetCreated {path, uuid}` (file write, then one undo step "Create Prefab X" that sets `prefabReference`), `InstantiatePrefab(uuidOrPath, parentId, siblingIndex, hasPosition, position)` -> `EntityCreated` (wraps `PrefabManager::InstantiatePrefabs`; one undo step; refs inside kept, outside empty; `position` is a world position), `ApplyPrefab(entityId)` (overwrites the file, keeps the prefab's names, no scene change) and `UnpackPrefab(entityId)` (one undo step). `HierarchyNode` and `EntityHeader` gain an optional `prefab`; Hello advertises `prefabAssets`. `CreatePrefab` and `ApplyPrefab` are refused by a play host; none is offered to Lua extensions. No overrides and no propagation to existing instances (EN4). FE-S3 syncs next.
- J9 (engine half) implements protocol 1.20.0: `GetPhysicsBackends` (`0xAD`, Read) and `SetPhysicsBackend(backend)` (`0xAE`, Write), both answering `PhysicsBackends` (`0x23`, `PhysicsBackendsInfo {current, configured, active, restartRequired, networkPreset, backends[]}`; each backend has `id`, `displayName`, `compiledIn`, `determinism`, `determinismDefault`, `features`, `missingFeatures`, `supportsSnapshot`, `supportsSetState`, `deterministicSelectable`, optional `note`). The setter writes `physics.backend` through the `SetProjectSettings` path, takes effect at the next play session or host start, and refuses an unknown name, a backend not compiled into the host, and (under `network.preset` deterministic) a backend that is not `deterministicSelectable`. Hello advertises `physicsBackends`; a play host refuses the setter. The frontend picker and the Deterministic card (enabled only when `deterministicSelectable`) are the frontend sync.
- FE-S1 is next after EN1 merges: update the engine pin/generated protocol and Lua API indexes, capability-gate the lint adapter, and wire the host wireframe and preview controls. The frontend PR deliberately retains its current engine pin; the editor API completion index records that the legacy pin lacks `editor-api.lua`.
- Orthographic picking has explicit host regression coverage, including parallel rays with different origins. CodeMirror's measured bundle increase is 253,893 gzip bytes; MIT dependency notices are included in the frontend.

Inputs it must stay consistent with:
- `proposals/lua-editor-extensions.md` (E-X1..E-X7; E-X2 is in flight and takes protocol **1.14.0** [M: progress log 2026-10-20]; E-X4 is the frontend PR: Tools menu, `FormDialog`, trust prompt, admin Extensions dialog).
- `proposals/netcode-runtime-design.md` sections 5.1-5.3 (A1: the `network` settings block and preset table; B11: the editor Network panel, a protocol minor).
- `proposals/webgpu-backend-design.md` (G0-G4 touch `renderer/`; anything here that adds a renderer API must be implemented in every backend).
- Memory: the frontend has CI (typecheck, build, Node tests, DOM tests, protocol-regeneration check) and the engine host does not build on the development machine, so everything visual needs the user's live run.

---

## 0. Executive summary

**The editor is further along than the candidate list assumes** [V]. Already done and dropped from this plan: multi-select in the hierarchy (click, Ctrl, Shift, arrows) and in the viewport (Ctrl/Shift pick toggles), a translate gizmo that moves the whole selection as one undo step with Ctrl snapping (fixed 0.5), multi delete/duplicate/reparent as one step each, frame selected (F), a selection box, console level and text filters, an undo/redo menu fed by `GetHistory`, and an engine health panel. What is missing is the rest of a "Unity-like" editor: rotate and scale gizmos, local space, configurable snapping, any persistence of layout or filters, a project settings UI (the commands are already allowlisted and unused), typed import settings, previews, a history list, hierarchy search, prefabs, multi-edit in the inspector, copy/paste, marquee select, a real code editor, viewport options and a profiler.

**Recommendation in one paragraph.** First a small frontend refactor (**F1**: a panel registry with tab stacks in the existing regions, plus persisted layout), because almost every new feature is a panel and E-X4 and B11 also add UI. Then a wide, parallel set of **frontend-only** features that need no protocol change (gizmos, snapping, overlays, camera bookmarks and orthographic views, marquee select, hierarchy search, console filters, history panel, project settings panel, import settings form, inspector multi-edit, a CodeMirror 6 script editor with completion from `engine-api.lua`). Then **three engine protocol minors**, sequenced after E-X2/E-X3: an "editor tooling" minor (wireframe view option, Lua syntax check, texture and audio previews), a copy/paste minor (serialize and instantiate entity subtrees), and a prefab-assets minor built on the same primitive. Prefab overrides/variants and an engine stats/profiler command come last: the first needs an engine data-model change, the second touches every renderer backend and should wait for the WebGPU stages.

Phases (section 16): **P0** F1-F2 foundations; **P1** frontend-only features in six parallel lanes; **P2** engine minors EN1-EN3 with their frontend syncs; **P3** prefab overrides, engine stats/profiler, and the slots E-X4 and B11 fill.

---

## 1. What exists today

### 1.1 Feature inventory [V]

| Area | Today | Where |
|---|---|---|
| Layout | Fixed: assets (left) / viewport over a bottom tab bar (console + text files) / hierarchy, inspector, engine (right). Splitter sizes are module-level signals "kept for the session": nothing persists, no panel can move, close or be re-opened | [FE] `src/renderer/editor.tsx:20-25`, `:403-428` |
| Panels | `Panel` is a titled `<section>` with actions; one file per panel; `panels.tsx` holds only `EnginePanel` | [FE] `src/renderer/ui.tsx:56-75`, `src/renderer/panels.tsx:9-74` |
| Menus | Flat `MenuItem` (label/action/disabled or separator), no submenus, no accelerators shown except in labels | [FE] `src/renderer/ui.tsx:224-232` |
| Shortcuts | Per-component `keydown` listeners: Ctrl+S and Undo/Redo in `editor.tsx:350-395`, F2/Del/Ctrl+D/arrows in `hierarchy-panel.tsx:111-122`, F/Esc/fly keys in `viewport-panel.tsx:286-323`. No registry, nothing rebindable |
| Gizmo | Translate only: three world-axis arrows plus three plane squares; no local space, no rotate/scale | [FE] `src/renderer/viewport-gizmo.ts:1-11`, `:35-94` |
| Snapping | Ctrl while dragging snaps the **delta** to `SnapStep = 0.5` along the moved axes; not configurable | [FE] `viewport-gizmo.ts:259-263`, `:491-500` |
| Viewport input | Alt+LMB orbit, MMB pan, wheel zoom, RMB look + WASDQE fly (only while RMB is held), F frame; `shortcutFor` knows only F | [FE] `src/renderer/viewport-input.ts:1-8`, `:57-68`, `:110-117`; `viewport-panel.tsx:307` |
| Camera | Perspective only; the request's orthographic fields are sent at defaults | [FE] README "The viewport" (line 223); protocol `SetEditorCamera` supports `orthographic`/`orthoSize` (`editor-server/protocol/protocol.json:140`) |
| Picking | Click pick (`PickEntity`, single), Ctrl/Shift toggles; no marquee | [FE] `viewport-panel.tsx:264-270`; `protocol.json:173` |
| Selection box | AABBs of the whole selection from one `GetEntityBounds` call, drawn on a 2D overlay canvas | [FE] `viewport-panel.tsx:132-154`; `protocol.json:190` |
| Multi-select | Hierarchy click/Ctrl/Shift/arrows; viewport toggle; translate moves all topmost selected objects as one group; multi delete/duplicate/move as one step | [FE] `hierarchy-state.ts:189-213`, `:362-439`; README "Translate gizmo" |
| Inspector | Primary selection only (a multi-selection shows the primary and a "Delete N Selected" button). Transform row uses the legacy `GetEntityTransform`/`SetEntityTransform` (Euler `vec3`, documented as the world transform), while the gizmo writes `SetLocalTransform` (quaternion) | [FE] `inspector-panel.tsx:320-350`; `scene-state.ts:13-20`, `:69`, `:92`; README line 180 |
| Console | Level checkboxes with counts, case-insensitive text filter, Clear; entries carry `source: host/editor/game` but there is no source filter; nothing persists; 5000 lines | [FE] `console-panel.tsx:39-56`; `console-store.ts:24`, `:62-75` |
| History | `EditorStore.history` holds cursor/labels/counts for the Edit menu; README: "Not here: a history panel (`GetHistory` has every step with its size)" | [FE] `store.ts:235`, `:886-900`; README line 204 |
| Hierarchy | Tree, rename, active toggle, create presets, duplicate, delete, drag reparent; **no search/filter**; no row virtualisation | [FE] `hierarchy-panel.tsx:163`; grep for filter/search in `hierarchy-*.ts` finds none |
| Assets | Tree/flat search by path/UUID and type filter, detail with **raw JSON** import settings + Apply, create folder/script/scene, drag to inspector slots. "Not here: moving, duplicating, deleting and thumbnails" | [FE] README lines 210-215; `assets-panel.tsx` |
| Text editor | A `<textarea>` per tab in the bottom panel, Ctrl+S `WriteTextAsset`, line endings preserved, error log lines after a save, read-only in play mode | [FE] `assets-panel.tsx:254`, `:416`; README line 214 |
| Project settings | `getProjectInfo`, `setProjectSettings`, `setStartupScene` are in the page's command allowlist but **no renderer code calls them** | [FE] `src/shared/api.ts:224-226`; grep in `src/renderer` finds no caller |
| Engine panel | `GetEngineHealth` list with Refresh; nothing else measured | [FE] `panels.tsx:9-74` |
| Persistence | Only the audio mute flag, in `localStorage` | [FE] `audio-controller.ts:12`, `:20` |
| Windows | One `BrowserWindow`, sandboxed, CSP `script-src 'self'; style-src 'self' 'unsafe-inline'` | [FE] `src/main/main.ts:118-130`; `src/index.html:5` |
| Theming | One hard-coded dark stylesheet in `index.html`; no CSS custom-property theme layer was found (grep for `:root`/`--` found none) | [FE] `src/index.html` |
| Accessibility | 75 `role=`/`aria-` attributes across the renderer; tabs and dialogs have roles; the viewport has `aria-label` | [FE] grep count |

### 1.2 Engine side that the plan builds on [V]

- **Protocol 1.13.0** (`editor-server/include/editor-server/Protocol.hpp:14`); `Capabilities()` still returns an empty list (`editor-server/src/EditorServer.cpp:2494-2497`).
- **Project settings**: `ApplyProjectSettings` blocks `layers`, `input` (`actionMaps`), `physics` (`fixedTimestep`, `gravity`, `gpu`, `deterministic`), `math` (`simdTier`), `determinism` (`luaSeed`, with a security warning), `rendering` (`colorSpace`, `forceShaderEncode`); `gpu`, `deterministic`, `simdTier` and `luaSeed` are read before `Application::Init`, so changing them **needs a host restart**; other blocks (`window`, unknown keys) are kept untouched (`engine/include/engine/ProjectSettings.hpp:18-62`, `engine/src/ProjectSettings.cpp:104-196`). `SetProjectSettings` is an RFC 7386 merge patch, all-or-nothing with rollback, class **Write** (an editor role may call it) (`protocol.json:660`, `EditorServer.cpp:3894-3895`). It is not part of undo (`Undo` lists the undoable commands, `protocol.json:690`).
- **Import settings**: `customData` keys are code-only: `texture` {`srgb`, `filter` linear|nearest, `wrap` repeat|clamp, `mipmaps`, `flipY`} (`engine/src/rendering/Texture.cpp:103-143`), `model` {`scale` > 0, `importMaterials`, `generateNormals`/`generateTangents` never|ifMissing|always, …} (`engine/src/rendering/Model.cpp:185-240`, `assetimport/include/assetimport/ModelImporter.hpp:75-87`), `font` {`basePx`, `spreadPx`, `paddingPx`, `charset`} (`engine/src/text/Font.cpp:85-130`). No command describes these schemas. No audio import settings were found.
- **Assets**: `ListAssets` types are LuaScript, Texture, Font, AudioClip, Material, Model, Scene; **no Prefab type**; `ReadTextAsset` extensions do not include `.prefab` (`protocol.json:475`, `:512`). `GetAssetInfo` returns size, mtime, `loaded`, `customData`, sub-assets; nothing visual (`protocol.json:490`).
- **Prefabs**: `Prefab` (name + root, `Serialize`/`Deserialize`), `PrefabManager::InstantiatePrefab(json)` (fresh UUIDs, internal refs remapped, external refs nulled) and an overload with a scene resolver that keeps external refs (used by `DuplicateEntity`, `EditorServer.cpp:6053`); an **in-memory** name registry only (`engine/include/engine/prefabs/Prefab.hpp`, `PrefabManager.hpp`; `docs/serialization.html:452-475`). `GameObject::_prefabReference` exists and round-trips but "nothing else uses it yet" (`engine/include/engine/GameObject.hpp:53`, `engine/src/gameobject/GameObject.cpp:944-946`, `:1041-1043`, `docs/serialization.html:142`). Models instantiate through the same path (`engine/include/engine/rendering/Model.hpp:95`, `:169-175`). **No prefab command in the protocol.**
- **Rendering**: `RenderEditorView` draws the open scene from the editor camera with no options (`EditorServer.cpp:4562-4588`). `IRenderer::SetWireframe(bool)` exists and is implemented by OpenGL, Software and Vulkan (`renderer/include/renderer/common/Renderer.hpp:236`, the three backend headers). No line/debug-draw primitive; Vulkan has "the three built-in shaders only" (`renderer/include/renderer/vulkan/VulkanRenderer.hpp:127`).
- **Stats**: no profiler, frame-stats, draw-call or memory counters anywhere (grep for `DrawCall|FrameStats|RenderStats|Profiler|GetMemoryUsage` over engine/renderer/editor-server: no match). `Time` exposes delta/fixed/unscaled times (`engine/include/engine/Time.hpp:37-51`); health rows are Window, Renderer, Physics, Particles, SoftBodies, Audio, Scripting (`engine/src/Application.cpp:97-101`, `:302-308`); `GetPlayState`/`playState` carry frame and time (`protocol.json:807`).
- **Colliders** serialise `halfExtents` (Box), `radius` (Sphere), `radius`+`height` (Capsule), `offset`/`isTrigger` (all) (`engine/src/physics/BoxCollider.cpp:14`, `SphereCollider.cpp:12`, `CapsuleCollider.cpp:12-13`, `ICollider.cpp:21-23`), so `GetEntity` already gives a client what it needs to draw collider wireframes.
- **Lua annotations**: `engine/src/scripting/bindings/engine-api.lua` is a 2664-line `---@meta` LuaLS file with 59 `---@class` and ~480 `---@param`/`---@field` lines. `LuaSandbox` and `DeclarationEvaluator` now exist in the engine (`engine/include/engine/scripting/LuaSandbox.hpp`, `engine/src/scripting/DeclarationEvaluator.cpp`).
- **Web**: the web build does not build `editor-server` (`CMakeLists.txt:331-334`), so web stages never conflict with protocol work, only with `renderer/` changes.

### 1.3 Constraints every design below respects
1. The page has no Node and no file system; everything goes through the host (assets) or the main process (dialogs, settings) [V README "Architecture"]. A new command must be added to the generated client, `EngineCommandArgs`/`EngineCommands` and the main-process argument checks (`src/shared/api.ts`, `src/main/engine-ipc.ts`).
2. The CSP allows only same-origin scripts: any editor library must be bundled by esbuild (`scripts/bundle.js`) and must not need `eval`, remote loading or blob workers [V `index.html:5`].
3. jsdom has no layout, canvas or `DataTransfer` [V README "Tests and CI"]: canvas overlays and gizmos are tested as pure maths (the existing pattern), DOM tests cover forms, lists and enablement.
4. Every write affordance follows `canWrite`/`canEdit`/`readOnlyReason` and the write guard (read-only host, viewer role, play mode) [V README "Read-only hosts", "Users and roles"].
5. **Each protocol minor needs one frontend sync PR** (regenerate `protocol.generated.ts` and `test-vectors.json`, bump `engine-ref.txt`); two syncs in flight conflict on the same files, so syncs are strictly serial [V README "Protocol", CI job].

---

## 2. Foundation F1: panel registry, tab stacks and layout persistence (candidate 6)

**Current state** [V]: see 1.1 (fixed layout, session-only sizes).

**Design.**
- A `PanelRegistry` (`src/renderer/layout/panel-registry.ts`): `{id, title, icon, component, defaultRegion, singleton, closable, requires?: (store) => boolean}`. Existing panels register: `assets`, `viewport`, `console`, `hierarchy`, `inspector`, `engine`; text-file tabs stay dynamic in the bottom region.
- A `LayoutState` (pure, unit tested): four regions (left, center, bottom, right), each an ordered **tab stack** (or, for left/right, up to three stacked groups with split ratios), plus sizes. Operations: move a panel to a region or tab, close, reopen (Window menu), reset to default. Serialised as `{version: 1, regions: …}` and validated on load (unknown panel ids dropped, missing singletons re-added to their default region, so a stale layout can never hide the viewport).
- No docking library (U1): drag a tab header onto another region's tab bar or use a tab context menu "Move to ▸ Left/Right/Bottom". Floating windows are out of scope (section 15).
- Persistence through F2's prefs store, per project, plus a user-level "default layout"; **Window ▸ Reset layout** restores defaults.
- New panels in this plan (history, project settings, profiler, prefab overrides) and E-X4's Extensions dialog and B11's Network card all become registry entries, so they "slot in" without editing `editor.tsx`.

**Tests.** Node: layout validation/migration, every operation, round-trip. DOM: move a panel via the menu, close/reopen, reset, restore from a stored layout, a stored layout naming an unknown panel. **User:** drag feel, splitter limits at small window sizes.

**Risks.** Touches `editor.tsx`, `panels.tsx`, `ui.tsx` and the CSS: the files E-X4 also edits (Tools button in the toolbar, `FormDialog` in `ui.tsx`). Landing F1 **before** E-X4 starts avoids a rebase of E-X4 onto a moved toolbar (U21).

**Size** M [E ~900 lines with tests]. **Deps:** none. **Order:** first.

## 3. Foundation F2: editor prefs store and keymap

- `PrefsStore` (`src/renderer/prefs.ts`): typed, versioned keys in `localStorage`, scoped `user` or `project:<projectId>` (the id from `GetProjectInfo.project.projectId`, so a remote host's project gets its own scope on this machine). Every read/write in try/catch with defaults (localStorage can be empty or throw). Holds layout, gizmo mode/space/snap settings, console filters, camera bookmarks, hierarchy search history (U2).
- `Keymap` (`src/renderer/keymap.ts`): a registry of named actions (`edit.undo`, `viewport.frame`, `gizmo.translate`, …) with default chords, a scope (global, viewport, hierarchy, text) and one dispatcher that respects `isTextEntry` and `modalOpen()` [V `edit-actions.ts`, `ui.tsx:278`]. F2 migrates only the global handlers in `editor.tsx`; panels move to it when their lane touches them (avoids cross-lane conflicts). A rebinding UI is deferred (section 15).

**Tests.** Node: scoping, defaults, corrupt storage, chord matching, text-entry suppression. **Size** S-M [E ~500]. **Deps:** F1 (shares `editor.tsx`).

---

## 4. Candidate 1: rotate and scale gizmos, snapping, local/world, shortcuts

**Current state** [V]: translate only, world axes, Ctrl snaps the delta by 0.5, no toolbar (1.1). Multi-object translate with per-object parent-chain conversion already exists (`viewport-gizmo.ts:1-11`, README "Translate gizmo").

**Design.**
- **Modes**: Translate (W), Rotate (E), Scale (R), and Q = no gizmo (select only), as in Unity (U3). The fly keys W/E/Q are consumed only while the right button is held (`viewport-panel.tsx:307`), so there is no clash: outside a look gesture they switch mode; inside, they fly. Shortcuts are viewport/hierarchy-scope keymap actions, never fired in a text field.
- **Space**: World / Local toggle (X, as Unity's "toggle tool handle rotation"). Local orients the handles by the primary object's world rotation (from `worldMatrix`, normalising the columns to strip scale [M: exact only without shear; a sheared parent chain shows approximate axes]). Scale is always local (as Unity), so the toggle is disabled in scale mode.
- **Pivot**: Pivot (primary object's origin) / Center (centre of the selection's union AABB, already loaded for the selection box) toggle (Z).
- **Rotate**: three rings plus a screen-space ring; the drag angle is measured on the ring plane (ray-plane intersection, fallback to screen-space angle around the projected centre when the plane is edge-on, mirroring the translate plane rules). Every topmost selected object turns about the pivot: world position `p' = pivot + R(p - pivot)`, world rotation `q' = R q`, converted to local with the parent's world rotation (from the parent `GetEntity` the translate drag already reads) and sent with `SetLocalTransform` (quaternion: no Euler drift).
- **Scale**: three axis handles with cubes plus a uniform centre handle; per object the local scale is multiplied along its own axes; with several objects and a uniform drag, offsets from the pivot scale too. Negative scale allowed but a drag through zero is clamped to a small epsilon to avoid degenerate matrices [E].
- **Snapping**: a viewport toolbar group: snap toggle per mode, increments (translate 0.5 default, rotate 15°, scale 0.1; U4), "relative" (snap the delta, today's behaviour) or "absolute grid" (snap world position to the grid, translate only). Ctrl inverts the toggle while dragging (today Ctrl turns it on). Stored per project (F2).
- **Undo**: one `BeginEditGroup`/`EndEditGroup` per drag with labels "Rotate <name>", "Scale N objects"; Esc restores as translate does. All the translate rules (newest-wins batches, abandon on full scene replace, frozen camera) are reused by extracting a shared `TransformDrag` base from `GizmoDrag`.
- **Inspector transform (U5)**: switch the inspector row to the local transform from `GetEntity` and write `SetLocalTransform`, showing Euler angles converted on the client and keeping the typed Euler while the quaternion is unchanged (so 180° doesn't flip to -180°). Removes the world/local mismatch between inspector and gizmo.
- **No engine or protocol change.**

**Tests.** Node: ring/handle layout and hit tests, angle from ray (edge-on fallback), rotation about a pivot through mirrored and rotated parents (exact against the engine's `Transform::Combine` rule already mirrored for translate), scale with negative and zero scales, snapping in both modes, group open/close and Esc. DOM: toolbar buttons and their disabled states (read-only host, play mode), keyboard mode switches not firing in a text field. **User:** feel and visual correctness against a live host (the README already lists the translate gizmo as unverified live).

**Risks.** Rotation of a child of a non-uniformly scaled parent cannot be represented exactly (shear); document it (Unity has the same limitation [M]). Gizmo code grows: keep geometry, hit test and drag maths pure as today.

**Size/PRs.** FE3 rotate/scale maths + drags (L [E ~1500 with tests]); FE4 viewport toolbar, modes, space/pivot, snapping settings, shortcuts (M [E ~700]); FE5 inspector local transform (S-M [E ~400]). **Deps:** F1/F2 (toolbar prefs, keymap); FE4 after FE3; FE5 independent (inspector lane).

## 5. Candidate 2: project settings panel

**Current state** [V]: commands allowlisted, no UI (1.1); blocks and restart rules in 1.2.

**Design.**
- A registry panel "Project Settings" (default: a tab in the right region; also **Edit ▸ Project Settings…**). Left: a section list; right: the section form. Sections v1 (U6):
  - **General**: name (read-only), project id, engine version, startup scene (scene picker → `SetStartupScene`).
  - **Physics**: fixed timestep (s, > 0), gravity (vec3), GPU (off/auto/on, *restart*), deterministic (bool, *restart*).
  - **Rendering**: colour space (gamma/linear), forceShaderEncode (advanced, collapsed).
  - **Math**: SIMD tier (*restart*).
  - **Determinism**: Lua seed (unset or integer ≥ 0, *restart*) with the engine's security note verbatim ("a constant hash seed makes hash flooding possible…").
  - **Layers**: 32 names (+ the collision matrix as a 32×32 checkbox grid in a second PR).
  - **Input**: `actionMaps` as a validated JSON editor in v1 (a structured editor later).
  - **Raw**: the whole `settings` object as JSON, for unknown blocks (read-mostly, explicit Apply).
- **Data flow**: `GetProjectInfo` on open and after each apply; edits are held locally per section; **Apply** sends only the changed keys as a merge patch (`null` for a cleared value), the answer replaces the form. Errors name the block (the host rolls back), shown under the section. A field marked *restart* shows "Takes effect after the host restarts" and, for a local project, a **Restart host** button (existing `store.restartHost()`, which already asks about unsaved scene changes [V README "The hierarchy and scenes"]).
- **Field schemas**: client-side `FieldSchema` objects per section, rendered with the existing `FieldEditor` (`inspector-editors.tsx`) so validation and widgets are shared [V `FieldEditor` is entity-independent, per lua-editor-extensions.md 1.2].
- **Slots for later work**: the section list is itself a registry (`settingsSections.register({id, title, order, component, requires})`). B11 registers **Network** (preset cards None / Server authoritative / Deterministic with the disabled reason, tunables, validation report from the `Network` health row; netcode-runtime-design 5.1-5.3) and E-X4 registers **Editor Extensions** (enable state, list, Reload for admins, trust status) as sections, not as separate dialogs. Neither needs this panel's code to change.
- **Roles**: viewer → read-only form; play mode → read-only (the play host refuses project writes and the edit host is not touched in play mode, but changing settings mid-play is confusing) [V `Undo` description lists `SetProjectSettings` as refused by a play host].
- **No engine change** for v1.

**Tests.** Node: the merge-patch builder (changed keys only, nulls), per-section validation, restart detection. DOM: load, edit, Apply sends the patch (fake engine records), host refusal shown, restart note, viewer/read-only disabled, registry section appears. **User:** a live host round trip (apply physics gravity, restart-needed fields).

**Risks.** `luaSeed` is a Write-class setting with a security trade-off; flag to the engine side whether it should be Admin (section 17). Input maps' shape is engine-defined and large; JSON v1 is honest.

**Size** FE9 M-L [E ~1100]; FE9b layers collision matrix S. **Deps:** F1. **Conflicts:** B11's frontend PR and E-X4 must come after FE9.

## 6. Candidate 3: import settings UI and asset previews

**Current state** [V]: raw JSON editor + Apply (README line 211), schema keys in 1.2, no previews.

**Design: import settings.**
- v1 (U7): a client-side schema table per resource type (Texture, Model, Font) mirroring the engine parsers listed in 1.2, rendered with `FieldEditor`; unknown keys are preserved and shown in an "Other (JSON)" box; "Edit as JSON" toggles to today's editor. Apply still sends the whole object (`SetImportSettings` replaces) [V `protocol.json:503`].
- v2: a host command `GetImportSettingsSchema(type)` returning `FieldSchema[]` from the engine's parsers, so the table cannot drift. Only worth it if the parsers grow; folded into EN1 as optional.

**Design: previews.** The host can provide nothing visual today. Proposed `GetAssetPreview(uuid, maxSize)` in EN1:
- **Texture**: decode and box-downscale to ≤ `maxSize` (≤ 256) RGBA; no GPU needed [E].
- **AudioClip**: `{durationMs, channels, peaks: [min,max]×N}` for a waveform strip (N ≤ 512) [E].
- **Font**: deferred (render "Aa" from the atlas later).
- **Model/Mesh/Material**: deferred (U8): rendering needs an isolated preview scene and a second render target, and `RenderEditorView` shares one render target with the viewport (resizing it moves the frame revision) [V `EditorServer.cpp:4562-4588`]; that is an engine design of its own and should wait for the WebGPU work.
- Caching: the client caches by `uuid + modified`; the host caches nothing in v1. Class **Read**, bounded CPU (decode once per request; the texture is not kept loaded).
- UI: a 64-px thumbnail column/grid toggle in the assets panel, a large preview + waveform in the detail pane, generic icons for types without previews.

**Tests.** Node: schema tables against the engine's documented keys (fixtures), preserving unknown keys, preview answer validation, cache invalidation. DOM: form ↔ JSON toggle, Apply payload. Host (EN1): preview of a PNG/JPG fixture (size, aspect), audio peaks of a WAV fixture, refusal for non-indexed paths, viewer allowed. **User:** thumbnails on a real project.

**Size** FE10 M [E ~700]; previews UI in the EN1 sync FE-S1. **Deps:** none for FE10; EN1 for previews.

## 7. Candidate 4: history panel

**Current state** [V]: history cursor/labels in the store; `GetHistory` gives every `{label, bytes}` and the cursor; at most 200 steps / 128 MiB; cleared when a scene is opened (`protocol.json:734`).

**Design.**
- Registry panel "History": rows oldest → newest, done steps normal, undone steps dimmed, the cursor as a marker, step memory on hover, total at the bottom; an "Initial state" row at the top.
- Clicking a row jumps: `EditController` gains `jumpTo(cursor)`, which settles pending edits and closes open groups once (the existing pre-undo sequence, [V README "What runs before the host is asked"]) and then sends `cursor - target` `Undo`s or `Redo`s sequentially, stopping at the first error. Each step is a `sceneChanged`; the panels already follow. One jump at a time; the panel is disabled while it runs (U19).
- Refresh: on `historyChanged` (debounced 100 ms) re-read `GetHistory` (the event has counts and labels, not entries) and on connect.
- Edit groups appear as their single labelled step (the host merges them), so a gizmo drag is one row; a jump never lands inside a group because groups are closed first.
- Optional later engine command `UndoTo(cursor)` only if long jumps over snapshot steps (`full: true` rebuilds) are slow (EN-later).
- Disabled whenever undo is (`canEdit`).

**Tests.** Node: jump planning (forward/back/no-op), stop on failure, settle-before-jump order with fake groups. DOM: rows, dimming, click → recorded Undo/Redo calls, disabled in read-only. **User:** a long jump on a live host.

**Size** FE8 M [E ~600]. **Deps:** F1. Independent of other lanes (touches `edit-controller.ts`, a new panel file).

## 8. Candidate 5: hierarchy search/filter and console filters

**Hierarchy** (current state: none [V]).
- A search box in the panel header: plain text matches names (case-insensitive); prefixes `t:Type` (component type names, available in `HierarchyNode.components` [V `protocol.json:930`]), `tag:`, `layer:`; results shown as a flat list with each match's path, or as the tree filtered to matches plus ancestors (toggle). Selecting a match reveals it (existing `reveal`). Keyboard: Ctrl+F focuses the box (keymap scope hierarchy). Last query per project in prefs.
- Pure `filterHierarchy(tree, query)` in `hierarchy-tree.ts`. No protocol change. Row virtualisation for big scenes is a separate gap (section 15).

**Console** (current state: level + text [V]).
- Add: a source filter (Host / Game / Editor, and "Extension" once E-X4 adds the tag), "match case" and "regex" toggles (regex errors shown inline), **Collapse** identical consecutive lines with a count, and persistence of all filter state per project. Optional: copy selected lines.
- Engine log lines have **no category/tag** today [V `EditorEvent` fields, `protocol.json:857`]. A category field would be an additive optional JSON field on the log event plus engine `Logger` categories; deferred (U20). E-X3's `extensionOutput` brings the first real tag.

**Tests.** Node: query parsing and matching, filtered-tree ancestors, console source/regex/collapse, persistence round trip. DOM: typing filters rows, clearing restores, invalid regex message.

**Size** FE6 hierarchy S-M [E ~450]; FE7 console S [E ~350]. **Deps:** F2 (prefs). **Conflict:** FE7 and E-X4 both touch `console-store.ts`; land FE7 first and let E-X4 add the Extension source.

## 9. Candidate 6: layout persistence
Covered by F1 (section 2): per-project layout + user default + reset. Nothing else to add.

## 10. Candidate 7: prefabs

**Current state** [V]: engine instantiation exists; no prefab asset type, no loader, no protocol; `_prefabReference` unused (1.2).

**Design v1 (U14, U15).**
- **Asset**: `res://…/*.prefab`, JSON in `Prefab::Serialize` format, indexed by a new loader so `ListAssets` shows type `Prefab` and it gets a `.meta`/UUID. It is **not** a `WriteTextAsset` extension: prefab files are written only by the commands below (so a malformed prefab cannot be hand-planted through the text editor, and the depth caps of `docs/serialization.html:155-161` apply on load as for scenes).
- **Commands** (one protocol minor, EN3; ids from a free block chosen at implementation time):
  - `CreatePrefab(entityId, path)` → `{uuid}`: serialises the subtree, writes the file (refusing an existing path unless `overwrite`), and links the source object (`prefabReference = uuid`) as one undoable scene edit ("Create Prefab X"). The file write itself is not undoable (like `SaveSceneToFile`). Class Write.
  - `InstantiatePrefab(uuidOrPath, parentId, siblingIndex, position?)` → `{entityId}`: `PrefabManager::InstantiatePrefab` (external refs null, as documented), sets `prefabReference` on the instance root, records one undo step like `DuplicateEntity`. Class Write.
  - `ApplyPrefab(entityId)`: overwrites the prefab file from the linked instance (asks on the client). `UnpackPrefab(entityId)`: clears the link (undoable).
  - `HierarchyNode`/`EntityHeader` gain an optional `prefab` (asset UUID) for the indicator (additive JSON).
  - `assetsChanged` follows file writes (existing watcher).
- **UI (FE-S3)**: prefab rows in the assets panel (icon, preview later); **drag a prefab** onto the hierarchy (drop position → `parentId`/`siblingIndex`, reuse the drop-target rules) or onto the viewport (position = `PickEntity` hit point, else a point in front of the camera); several dragged prefabs in one `BeginEditGroup`; **Create Prefab** in the hierarchy context menu (path prompt defaulting to `res://prefabs/<name>.prefab`) and by dragging a hierarchy row onto an assets folder; instance rows show a prefab badge; the inspector header shows "Prefab: X" with Select asset / Apply / Unpack.
- **v2: overrides** (EN4, design later): diffing an instance against its prefab needs a stable correspondence between instance objects/components and prefab objects, which fresh-UUID instantiation destroys. It needs a per-object `prefabSourceId` (serialised), a host `GetPrefabOverrides(entityId)` (field-level diff), `RevertPrefabOverride`, an **overrides indicator** (bold fields, a dot on rows), and propagation of prefab changes into instances when a scene loads. **v3**: nested prefabs and variants. Both are engine data-model work with save-format consequences: plan only after v1 is used.
- **Copy/paste shares the primitive** (section 11): `InstantiatePrefab` and paste both build a subtree from JSON; implement EN2's `InstantiateEntities` first and make `InstantiatePrefab` a thin file-loading wrapper.

**Tests.** Host (EN3): create → file content and link; instantiate → fresh UUIDs, link set, one undo step, undo removes; refs inside kept, outside null; apply/unpack; path checks; viewer refused; play host refuses file writes; depth-capped file refused. Frontend: drop-target → arguments, grouped multi-drop, badge rendering, menu enablement (Node + DOM with a hand-made `dataTransfer`, as the hierarchy DOM tests do). **User:** drag into viewport on a live host.

**Risks.** Users expect Unity's overrides immediately; v1 must say clearly that instances are copies with a link, and "Apply" overwrites the prefab. Changing a prefab does not update existing instances in v1.

**Size** EN3 L [E ~1500 engine incl. tests and docs]; FE-S3 L [E ~1200]. **Deps:** EN2 (shared instantiate primitive).

## 11. Candidate 8: multi-select (what remains)

**Already done** [V]: hierarchy Shift/Ctrl, viewport toggle, multi-translate, multi delete/duplicate/move (1.1). **Remaining:**
- **Marquee select** (FE12): left-drag on empty space (not on a handle, past the click slop) draws a rectangle; on release, select objects whose projected world AABB intersects (Alt: fully inside) the rectangle; Shift/Ctrl add/remove. v1 uses `GetEntityBounds` for every node of the hierarchy (≤ 4096 per call, the host's cap [V `protocol.json:190`]; chunk above that) cached per scene revision, so no protocol change (U12). Precision is AABB-level; an exact host `PickEntitiesInRect` is a later option only if users complain.
- **Multi-object rotate/scale** come with FE3 (section 4).
- **Inspector multi-edit** (FE11): when several objects are selected, show the common header fields (active, tag, layer, transform) and the component types present on **all** of them (matched by type, the first of each type); mixed values show "—"; an edit sends `SetComponentFields`/`SetEntityProperties`/`SetLocalTransform` per object inside one edit group ("Edit N objects"). Lua components only when they share the same script. Add Component adds to all; Remove removes from all (one group). Pure logic in `inspector-state.ts` (a `MultiInspector` view model), no protocol change.
- **Copy/paste/duplicate**: duplicate exists. Copy/paste needs host support because the page cannot build objects from JSON; EN2 adds `SerializeEntities(ids)` → `{json}` (Prefab-format array of the topmost selected subtrees, bounded size) and `InstantiateEntities(json, parentId, siblingIndex, keepExternalRefs)` → ids (one undo step, "Paste N objects"; `keepExternalRefs` uses the resolver overload when pasting into the same scene). Clipboard: an in-memory editor clipboard plus the system clipboard as text with a MIME-like header, so a paste into another project works (the host validates everything; JSON is untrusted, depth caps apply). Ctrl+C/Ctrl+V/Ctrl+X (cut = copy + delete in one group) in hierarchy/viewport scope (U13).

**Tests.** Node: rectangle/AABB projection (behind-camera boxes clipped like the selection box), add/remove semantics, multi-inspector common-field and mixed-value logic, edit fan-out in one group. DOM: inspector with two fake objects, mixed display, one edit → N recorded calls. Host (EN2): serialise/instantiate round trip, fresh UUIDs, refs, size limits, undo, viewer refused. **User:** marquee feel, paste across scenes.

**Size** FE11 L [E ~1300]; FE12 M [E ~600]; EN2 M [E ~800]; FE-S2 M [E ~500]. **Deps:** FE12 after FE3/FE4 (same viewport files); FE11 after FE5 (inspector files).

## 12. Candidate 9: script editor with Lua completion

**Current state** [V]: textarea editor (1.1); `engine-api.lua` annotations (1.2); no syntax check on the host (the protocol has none; a script's load error arrives as a log line after `WriteTextAsset`, README line 214).

**Library choice (U9): CodeMirror 6** [M for all library facts]:
| | CodeMirror 6 | Monaco |
|---|---|---|
| Size | modular, roughly 0.3-0.6 MB minified for editor + Lua mode + autocomplete + lint | several MB plus language workers |
| Bundling | plain ES modules, bundles with the existing esbuild script, no workers | needs worker entry points; default setup uses blob/`new Worker` URLs that the CSP would have to allow; heavier esbuild setup |
| Offline/Electron | fully local | fully local once bundled |
| Lua | `@codemirror/legacy-modes` Lua stream mode (highlighting); completion/hover/lint are extension points we feed | Monarch Lua tokenizer; no LuaLS without a language server |
| Licence | MIT | MIT |
| Testing in jsdom | partially works (no layout); logic stays testable as pure functions | poor |
Monaco's big advantage (VS Code feel, built-in language services) does not apply to Lua without running LuaLS, which this design avoids. CM6 fits the CSP and the bundle script unchanged.

**Design.**
- Replace `TextEditor`'s textarea with a CM6 view; keep `AssetsState` (tabs, dirty, line endings, conflict reload, read-only in play mode) as the owner of text, so its tests stay valid. Features: line numbers, Lua/JSON highlighting by extension, search/replace, bracket matching, Ctrl+S via the keymap, read-only state from `canWrite` and play mode.
- **API index (U10)**: extend `scripts/sync-protocol.js` (or a sibling `sync-lua-api.js`) to read `engine/src/scripting/bindings/engine-api.lua` from the pinned engine checkout and generate `src/renderer/lua/lua-api.generated.json` (classes, fields, functions, params, returns, doc comments) with a small LuaLS-annotation parser; CI's regeneration job checks it like the protocol files. Completion: globals and `Class.` members, `self.` inside a behaviour script's methods (the script template's shape), and the `SerializableFields` declaration keys; hover: the doc comment and signature. No type inference beyond that in v1.
- **Diagnostics (U11)**: EN1 adds `CheckLuaSyntax(text, chunkName)` → `{ok, line, column?, message}`: the host compiles with `luaL_loadbufferx(…, "t")` in a throwaway state and **never runs** the chunk; Read class; 4 MiB cap as `ReadTextAsset`. The editor calls it 500 ms after typing stops (latest-wins sender) and shows a squiggle via `@codemirror/lint`. Before EN1 lands, the existing post-save log lines stay. A JS Lua parser was rejected because it would not match Lua 5.4 exactly (attributes `<const>`/`<close>`, integer division) [M].
- **Roles**: viewer and read-only host → read-only editor (syntax check still allowed: Read).
- **Lua extensions (E-X4..E-X7)**: extension files live in `<project>/editor/` outside `assets/`, unreachable by `ReadTextAsset`/`WriteTextAsset` by design (lua-editor-extensions D2). So this editor **cannot open extensions** and must not try; a future admin-only read/write command (deferred there) would plug in as another tab source. When E-X7 ships `editor-api.lua`, the sync script indexes it too and the completion set is chosen by the file's location (extensions vs scene scripts). The Tools menu, `FormDialog` and the trust prompt are independent of this editor; the shared touchpoint is `ui.tsx` (dialogs) and the keymap (Tools items can register shortcuts later).

**Tests.** Node: the annotation parser (fixtures from `engine-api.lua` snippets: `---@class`, `---@field`, overloads, optional params), completion context detection, lint mapping, latest-wins check scheduling. DOM: editor mounts, read-only follows the guard, Ctrl+S saves through the fake engine. Host (EN1): syntax error line/col, valid chunk, `while true do end` returns ok without hanging (compile only), oversize refused. **User:** typing feel, completion in a real script, bundle size check.

**Size** FE14 L [E ~1400 + vendored deps]; FE-S1 adds the lint hookup (S). **Deps:** none for FE14 (assets lane, after FE10 to avoid `assets-panel.tsx` conflicts); EN1 for diagnostics. **Licences:** add CM6 to the frontend's notices if it keeps one [M].

## 13. Candidate 10: scene view overlays and viewport options

**Current state** [V]: overlay canvas with selection box and gizmo; perspective only; F frame; no options.

**Design.**
- **Client-drawn overlays** (FE13a, no protocol, U17), toggled from a viewport "Gizmos ▾" menu and saved per project:
  - **Grid** on the XZ plane (or the view plane in orthographic axis views), fading with distance, drawn on the overlay canvas. Limitation: no depth occlusion (objects do not hide it). A host-drawn grid would need a line primitive or a custom shader, and Vulkan has only built-in shaders [V `VulkanRenderer.hpp:127`]; defer.
  - **Collider wireframes** for Box/Sphere/Capsule from `GetEntity` values (`halfExtents`, `radius`, `height`, `offset`) and `worldMatrix` [V 1.2]; for the selection always, for all objects optionally (reads bounded and cached by revision).
  - **Bounds** for all objects (not only the selection), **light/camera icons** and light ranges (point radius, spot cone) from component values.
- **Host options** (EN1): `SetEditorViewOptions({wireframe})` → the editor view calls `IRenderer::SetWireframe` around `RenderEditorFrame` [V the API exists in all three native backends; WebGPU G-stages must implement it anyway as it is pure virtual]. The frame revision moves when options change. Shading modes beyond wireframe (unlit, normals, overdraw) need shader work: deferred.
- **Camera** (FE13b, no protocol): **orthographic** toggle (the protocol supports it [V `protocol.json:140`]; `viewport-math.ts`, `screenRay` and the gizmo maths must handle an orthographic projection: rays become parallel), **axis views** (top/front/right and opposites from a small view cube or a menu; numpad-style shortcuts), a **2D mode** = orthographic + front view + rotation locked + pan/zoom only, **camera bookmarks** (save/restore up to 9 poses per scene, Ctrl+Shift+1..9 / Shift+1..9, stored per project), and "align view to selected". Picking in orthographic depends on the host casting from the orthographic camera [M: `PickEntity` says "from the editor camera"; verify with a test in EN1 or the first live run].
- **Focus/frame** already exists [V].

**Tests.** Node: grid line generation and fading, collider wireframe geometry per shape through a scaled/rotated `worldMatrix`, orthographic matrices/rays/gizmo hit tests, bookmark store. DOM: menu toggles persist. Host (EN1): wireframe toggles move the revision; an orthographic pick test. **User:** overlay look, 2D mode feel.

**Size** FE13a M [E ~800]; FE13b M [E ~800]. **Deps:** viewport lane after FE3/FE4/FE12.

## 14. Candidate 11: profiler/stats panel

**Current state** [V]: only `GetEngineHealth` subsystem states; no counters in the engine (1.2). The edit host does not tick [M lua-editor-extensions 1.1], so meaningful frame stats come from the **play host**.

**Design.**
- **Now (FE15, no protocol, U18)**: client-side stats in the Engine panel: editor frame request latency and bytes/s, frames/s shown, `PollEvents` lag, audio underruns/buffer level (already counted [V README "Audio"]), play state frame/time rate (frames per wall second from `GetPlayState`).
- **Later (EN5)**: `GetEngineStats` (play and edit host): frame time last/avg/max over a window, update/fixed/physics/render ms (scoped timers in the main loop), draw calls/triangles (counters in each renderer backend), object/component counts, Lua memory (`collectgarbage("count")` equivalent on the C side), process memory (platform calls), physics stats (PhysX scene statistics [M]). Then a registry panel "Profiler" with sparklines (ring buffer, 1 Hz to 10 Hz polling of the play child through `PlayCommandArgs`). Draw-call counters touch every backend including WebGPU, so EN5 waits for the WebGPU G-stages to settle.
- Netcode's `Network` health row (A1) appears in the Engine panel automatically (it lists rows generically) [V `panels.tsx:46-56`]; the B11 network stats overlay (RTT, loss) can be a Profiler tab later.

**Size** FE15 S [E ~300]; EN5 L [E ~1500 engine]; FE-S4 M. **Deps:** EN5 after WebGPU G2+ (U18).

---

## 15. Other gaps found while surveying

| Gap | One line | Plan now? |
|---|---|---|
| Keyboard shortcut editor | Rebinding UI on top of F2's keymap, plus a shortcut cheat sheet (?) | Later (after F2; S) |
| Command palette / search everywhere | Ctrl+P over assets, objects, menu actions and settings sections | Yes, P1 tail (M) once the keymap and registries exist |
| Hierarchy/assets/console virtualisation | Rows render all at once; thousands of objects or 5000 log lines will be slow | Yes, small PR when a big scene is measured slow (S-M) |
| Asset move/rename/delete/duplicate | README lists them as not here; needs host commands (a move must keep the `.meta` UUID) | Plan with EN-later; high value (engine M + FE M) |
| Theming | One hard-coded dark sheet; move colours to CSS custom properties, light theme, high contrast | Later (S-M); prerequisite for accessibility contrast |
| Accessibility | Roles exist; missing: focus order across panels, visible focus rings audit, menus keyboard navigation, screen-reader names for canvas tools, reduced motion | Audit PR after F1 (S) |
| Error recovery | Error banner only; add a notifications list (history of errors), "copy diagnostics" (versions, protocol, last log lines) | Later (S) |
| Preferences dialog | `settings.json` (host path, ready timeout, renderer) has no UI except the host locator | Yes, small (S) next to the project settings panel |
| Multi-window | Pop a panel (game view, console) into a second window: new IPC routing per window, CSP and sandbox rules per window | No (L, low value now) |
| Drag and drop from the OS | Import files by dropping them on the assets panel: needs a host upload/import command | Later (engine + FE M) |
| Scene validation | "Missing references", "unregistered component types", "objects without transform" report, and E-X6's `OnBeforePlay` warnings | Later (host command M) |
| Play-mode inspector | Inspect the play child read-only (#6 decision 13 says the inspector is read-only while playing; today it shows the edit host) | Later (M), after multi-edit |
| Recent remotes for Direct (TLS) | README: no recent list for the direct form | Small FE item (S) |
| Undo of project settings | Not in the host history; a local "previous value" revert per section is enough | Part of FE9 |

---

## 16. Phases, PRs, parallelism and conflicts

Sizes: S < 400 changed lines, M 400-1200, L > 1200 [E]. "CI" = frontend CI (typecheck, build, Node tests, DOM tests, protocol check) or engine CI (10 checks, required `linux (Release, x86_64, Clang)` [M memory]). "User" = needs the user's live Electron + host run.

### Phase P0: foundations (now; frontend only; parallel to E-X2/E-X3 engine work and netcode A1/A2)
| PR | Repo | Content | Size | Deps | CI verifies | User verifies |
|---|---|---|---|---|---|---|
| F1 | FE | Panel registry, tab stacks, Window menu, layout persistence + reset (cand. 6) | M | none | layout model tests; DOM move/close/reopen/reset/restore | drag/resize feel |
| F2 | FE | Prefs store (user/project scopes) + keymap registry; migrate global shortcuts | S-M | F1 | scoping, corrupt storage, chord tests | shortcuts still work |

### Phase P1: frontend-only features (no protocol), six lanes in parallel after F2
| Lane | PRs in order | Files (conflict domain) |
|---|---|---|
| V viewport | FE3 rotate/scale maths+drags (L) → FE4 toolbar/modes/snapping/shortcuts (M) → FE12 marquee (M) → FE13a overlays (M) → FE13b ortho/2D/axis views/bookmarks (M) | `viewport-*.ts(x)` |
| I inspector | FE5 local transform in inspector (S-M) → FE11 multi-edit (L) | `inspector-*.ts(x)`, `scene-state.ts` |
| H hierarchy | FE6 search/filter (S-M) | `hierarchy-*.ts(x)` |
| C console | FE7 filters + persistence + collapse (S) | `console-*.ts(x)` |
| P panels | FE8 history panel (M) → FE9 project settings panel (M-L) → FE9b layers matrix (S) → FE15 client stats (S) | new panel files, `edit-controller.ts`, `panels.tsx` |
| A assets | FE10 typed import settings (M) → FE14 CodeMirror 6 script editor + Lua API index (L) | `assets-*.ts(x)`, `scripts/`, `package.json` |

CI verifies all logic and DOM; the user verifies FE3/FE4/FE12/FE13 visually and FE14's bundle and typing feel. The lanes touch disjoint files except `store.ts` (small additions: rebase-only conflicts) and `index.html` CSS (append-only sections per lane to keep merges trivial).

### Phase P2: engine protocol minors and their frontend syncs (strictly serial on the protocol)
Version slots [M]: E-X2 = 1.14.0 (in flight); E-X3 = the next minor (1.15.0 if it lands next); B11 = a minor when netcode reaches it. **Rule:** a PR takes the next free minor at merge time; each is followed by exactly one frontend sync PR; syncs never overlap.

| PR | Repo | Content | Size | Deps | CI verifies | User verifies |
|---|---|---|---|---|---|---|
| EN1 | engine | "Editor tooling" minor (U16): `SetEditorViewOptions{wireframe}`, `CheckLuaSyntax`, `GetAssetPreview` (Texture RGBA ≤256, AudioClip peaks), optional `GetImportSettingsSchema`; classes (Read except view options = Read too, view-only); vectors; docs | M-L | after E-X2 merges (protocol file conflicts) | engine unit + protocol vector tests; host tests listed in sections 6, 12, 13 | — |
| FE-S1 | FE | Sync EN1: wireframe toggle, lint in the script editor, thumbnails/waveforms in assets | M | EN1, FE14, FE10, FE13a | DOM + Node | thumbnails, wireframe, squiggles live |
| EN2 | engine | Copy/paste minor: `SerializeEntities`, `InstantiateEntities` | M | EN1 (serial protocol slot) | round trip, refs, undo, limits, roles | — |
| FE-S2 | FE | Sync EN2: Ctrl+C/V/X in hierarchy and viewport, clipboard format | M | EN2 | Node + DOM | paste across scenes/projects |
| EN3 | engine | Prefab assets minor: `.prefab` loader/type, `CreatePrefab`, `InstantiatePrefab`, `ApplyPrefab`, `UnpackPrefab`, `prefab` on hierarchy nodes; docs | L | EN2 | listed in section 10 | — |
| FE-S3 | FE | Prefab UI: asset rows, drag to hierarchy/viewport, create/apply/unpack, badges | L | EN3 | Node + DOM (hand-made dataTransfer) | drag into viewport live |

### Phase P3: later and externally owned slots
| PR | Repo | Content | Size | Deps |
|---|---|---|---|---|
| E-X4 | FE | (lua proposal) Tools menu, FormDialog, trust prompt; **Extensions** as a project-settings section; console "Extension" source | (as planned) | E-X3, F1, FE7, FE9 |
| B11-FE | FE | (netcode) Network section in project settings, nudge on first NetworkObject | M | B11 engine minor, FE9 |
| EN4 | engine | Prefab overrides v2 (source ids, diff, revert, indicator data, propagation) | L+ | EN3 + its own design doc |
| FE-S4 | FE | Overrides indicator and revert UI | M-L | EN4 |
| EN5 | engine | `GetEngineStats` (timers, draw calls in every backend, memory, physics, Lua) | L | WebGPU G2+ settled |
| FE-S5 | FE | Profiler panel | M | EN5 |
| later | both | `UndoTo`, log categories, asset move/rename/delete, OS file import, scene validation, command palette, shortcut editor, theming | S-M each | — |

### Parallelism and conflicts at a glance
- **Parallel**: all P1 lanes with each other; P0/P1 with every engine PR (E-X2, E-X3, A1, A2, EN1-EN3 drafting) and with the web/WebGPU stages (no shared files).
- **Serial**: F1 → F2 → lanes; within a lane, in the order listed; every protocol minor (E-X2, E-X3, EN1, EN2, EN3, B11, EN4, EN5) and every frontend sync PR (E-X4's sync, FE-S1..S5, B11-FE).
- **File conflicts to schedule around**: `editor.tsx`/`ui.tsx` (F1 vs E-X4: F1 first); `console-store.ts` (FE7 vs E-X4: FE7 first); project settings panel (FE9 vs E-X4 and B11-FE: FE9 first); `assets-panel.tsx` (FE10 → FE14 → FE-S1 → FE-S3); viewport files (lane V order, then FE-S1's wireframe button, FE-S2/S3 drop handling); `renderer/` (EN1 only calls an existing API; EN5 adds counters to every backend: after WebGPU).
- **Recommended run order** [E]: F1, F2; then FE3, FE5, FE6, FE7, FE8, FE10 in parallel; then FE4, FE11, FE9, FE14; then FE12, FE13a, FE13b, FE9b, FE15; EN1 as soon as E-X2 (and, if the protocol slot is taken, E-X3) merge; then EN2, EN3 with their syncs; P3 by demand.

---

## 17. Risks
- **Unverified UI accumulates.** Every viewport and drag feature is CI-tested only as maths; the live host does not build on the development machine. Mitigation: each visual PR lists a short live checklist for the user, batched into the existing user-verification list.
- **Protocol slot contention** with E-X2/E-X3/B11 and the sync PRs that each bump forces. Mitigation: batch EN1 (U16), keep syncs serial, never start a sync until its engine PR merged.
- **Layout refactor churn** (F1) before E-X4: if E-X4 is started first, it must rebase onto moved toolbar code.
- **Prefab expectations**: v1 is "linked copies"; overrides are a real engine data-model change (EN4) with a save-format migration.
- **`luaSeed` and other sensitive settings** are Write-class; the settings panel exposes them to the editor role. Worth asking the engine side whether `determinism` should be Admin-only (an engine question, not a UI one).
- **Bundle growth** from CodeMirror 6 (estimate 0.3-0.6 MB [M]); acceptable for Electron, measure in FE14.
- **Client-side overlays without depth** (grid) can look wrong over geometry; a host grid is a later renderer task.
- **Marquee at AABB precision** may select objects whose box but not mesh is inside; acceptable v1, documented.

## 18. Could not verify
- Whether `SetEntityTransform`/`GetEntityTransform` are world or local in the host: the README says world (line 180) and the handler calls `SetPositionAndRotation` with Euler angles (`EditorServer.cpp:~5143-5146`); not traced into `Positionable`.
- Whether `PickEntity` and `GetEntityBounds` behave correctly with an orthographic editor camera (the description says "from the editor camera"; no orthographic test was found or run).
- The protocol minor E-X3 will take (1.15.0 assumed) and B11's.
- CodeMirror 6 and Monaco sizes, CSP needs and Lua support (general knowledge, not measured here); whether a JS Lua parser handles Lua 5.4 attributes.
- Whether the frontend repo keeps a third-party notices file that new dependencies must be added to.
- PhysX statistics availability through the engine's physics backend interface (`IPhysicsBackend`), for EN5.
- How expensive texture decode for `GetAssetPreview` is on large textures (no measurement), and whether the texture loader keeps CPU pixels.
- Whether the play host's main loop has a natural place for scoped timers without touching determinism work (A2's `SimulationClock` also edits `Application.cpp`).
- The live behaviour of anything in the frontend (README marks the viewport, assets panel and remote paths as not verified against a live host).

## 19. Decisions for the user
- **U1 Layout model:** own lightweight panel registry with tab stacks in the four existing regions, persisted per project with a reset, no docking library and no floating windows **(recommended)**; or adopt a docking library.
- **U2 Where per-project editor state lives:** the renderer's `localStorage` keyed by the project's `projectId`, per machine **(recommended)**; or a file in the project written through the host.
- **U3 Gizmo shortcuts:** Unity's W/E/R with Q for select-only, X for local/world, Z for pivot/center, with the fly keys keeping priority while the right button is held **(recommended)**; or another scheme.
- **U4 Snap defaults:** translate 0.5, rotate 15°, scale 0.1, relative (delta) snapping by default with an absolute-grid option for translate, Ctrl inverting the toggle **(recommended)**; or other values.
- **U5 Inspector transform:** switch it to the local transform (`GetEntity` + `SetLocalTransform`, Euler shown, quaternion sent) so it matches the gizmo **(recommended)**; or keep the legacy world Euler commands.
- **U6 Project settings v1 scope:** General/startup scene, Physics, Rendering, Math, Determinism (with restart badges and the luaSeed warning), Layers, Input as JSON, Raw JSON, with Network (B11) and Editor Extensions (E-X4) added later as sections of the same panel **(recommended)**; or a smaller first cut.
- **U7 Import settings form:** a client-side typed schema table for Texture/Model/Font now, a host schema command only if the parsers grow **(recommended)**; or the host command first.
- **U8 Previews v1:** host `GetAssetPreview` for textures and audio waveforms, mesh/material/font previews deferred **(recommended)**; or include rendered mesh previews now.
- **U9 Code editor:** CodeMirror 6 **(recommended)**; or Monaco.
- **U10 Lua API source:** generate a completion index from `engine-api.lua` at sync time, pinned to `engine-ref.txt` and checked in CI **(recommended)**; or a host command serving the annotations.
- **U11 Lua diagnostics:** a host `CheckLuaSyntax` that compiles and never runs (Read class) **(recommended)**; or a JavaScript Lua parser in the page.
- **U12 Marquee select:** client-side from `GetEntityBounds` of all objects (AABB precision) **(recommended)**; or a new host `PickEntitiesInRect` first.
- **U13 Copy/paste:** new host commands `SerializeEntities`/`InstantiateEntities`, also used by prefab instantiation, with a system-clipboard text format **(recommended)**; or duplicate-only.
- **U14 Prefab v1 scope:** `.prefab` assets with create, instantiate (drag to hierarchy/viewport), apply, unpack and a link badge; overrides v2 and nested/variants v3 each get their own design later **(recommended)**; or design overrides before shipping v1.
- **U15 Prefab files:** JSON in `Prefab::Serialize` format under `assets/`, indexed with a loader, written only by the prefab commands and not by `WriteTextAsset` **(recommended)**; or editable as text.
- **U16 Protocol batching:** one "editor tooling" minor (wireframe option, syntax check, previews) instead of one minor per command **(recommended)**; or one minor each.
- **U17 Viewport overlays:** grid, collider, bounds and light/camera overlays drawn by the client now, the host only for wireframe; a host-drawn grid and shading modes later **(recommended)**; or host-side overlays from the start.
- **U18 Profiler timing:** ship client-side stats now and defer the engine `GetEngineStats` until the WebGPU stages settle, since draw-call counters touch every backend **(recommended)**; or start engine stats now.
- **U19 History jump:** sequential Undo/Redo from the client, a host `UndoTo` only if jumps prove slow **(recommended)**; or add `UndoTo` with EN1.
- **U20 Console categories:** client-side source filter now, engine log categories later as their own small minor **(recommended)**; or add categories in EN1.
- **U21 Order against E-X4:** land F1 (and FE7, FE9) before E-X4 starts, so the Tools menu, Extensions section and extension console source build on them **(recommended)**; or let E-X4 go first and rebase F1.
