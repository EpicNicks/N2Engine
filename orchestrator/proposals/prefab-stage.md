# Proposal: the prefab stage (edit a prefab by double-clicking it)

Status: design only, no code. Assumes **stable asset UUIDs** (asset-move work, PR #284). Protocol: this proposal takes **1.26.0**, after `prefab-references.md` (1.25.0), after MoveAsset (1.24.0). Renumber if the merge order changes; nothing else depends on the numbers.

Decided by the user: **both viewing modes ship in the first release**: **Isolated** (the prefab alone, default light, grid) and **In Scene / Context** (the prefab shown inside the current scene, the rest dimmed, not selectable, not editable), switchable from the stage toolbar; breadcrumb back to the scene; Save and Close with an unsaved-changes prompt.

## 1. What the host is today [V = verified in the tree]

- The host holds **one** scene: `SceneManager` has `_scenes` (stored JSON data) and one `_loadedScene` (`unique_ptr<Scene>`); `GetCurScene()`/`GetCurSceneRef()`. `EditorServer` remembers the file (`_openScene`, a `const Scene*` it was opened into) and `_sceneRevision` (`EditorServer.hpp` ~1419-1423) [V].
- Almost every edit handler resolves objects with `SceneManager::GetCurScene()->FindGameObjectByUUID(uuid)` (63 `Scene`/`GetCurScene` sites in `EditorServer.cpp`) [V]. Entity ids on the wire are GameObject **UUID strings**.
- Undo: one `EditHistory _history` (`EditHistory.hpp`): ops address objects **by UUID, looked up when they run**, and big changes restore a `SnapshotScene(loaded scene)` text via `RestoreSceneSnapshot` [V]. The history therefore follows "the loaded scene", whichever object that is.
- Rendering: `RenderEditorView` calls `Application::RenderEditorFrame(camera)` -> `Application::Render(camera, false)`, which draws `SceneManager::GetCurSceneRef()` through `Scene::CollectLighting()` and `Scene::Render(renderer, camera)`; no scene parameter [V, `Application.cpp` 832-1008]. The editor camera is host state (`EditorCameraState`), not a scene object, never saved, not in undo [V, `EditorCamera.hpp`].
- Picking (`Picking::`, `ScenePicking.hpp`) works on a loaded, never-attached scene: **edit mode never attaches components** (no physics bodies, no scripts), so a Scene object can exist without simulating [V].
- Play: the editor's play session is a **separate play host** (a child process fed a scene snapshot; `IsPlayMode()` is true only in that process) [V, `EditorServer.hpp` ~704-714]. It snapshots the loaded scene.
- Scene saving: `SaveSceneToFile`/autosave serialize the **loaded scene** (`Scene::Serialize`) [V]. Autosave writes under `.n2/autosave` keyed by the open scene's path.
- Prefab write path: `ApplyPrefab(entityId)` overwrites the `.prefab` from the linked instance, keeping the prefab's names, no scene change [V, roadmap 10]. `CreatePrefab`/`ApplyPrefab` are refused by a play host.
- Renderer: per-draw fixed-function state is `RenderState` (`renderer/include/renderer/common/RenderState.hpp`); it **travels with the draw** because a backend may reorder draws (the software renderer does), so a global setter cannot follow them [V]. Backends: OpenGL, Vulkan, WebGPU, software (`renderer/src/{opengl,vulkan,webgpu,software}`).

## 2. Architecture decision: swap the loaded scene (recommended)

Three candidate stages:

| Option | Isolated | In Scene | Verdict |
|---|---|---|---|
| A. Temporary subtree in the open scene | hide everything else | add the instance next to the rest | Every command works unchanged, but the stage objects live **in the scene**: they would be in `Serialize`, in autosave, in undo, in the play snapshot, in the revision. Needs a "never save" flag threaded through every path. A crash or a missed path leaves prefab junk in the scene file. Rejected. |
| B. A second Scene object handled by new code paths | own scene | own scene + draw both | Clean separation, but the 63 handler sites would all need "which scene" plumbing. Large and error-prone. |
| **C. Swap the loaded scene** | stage Scene becomes the loaded scene; the open scene is **parked** (owned by the host, untouched) | same, plus the parked scene is drawn behind it as a render-only backdrop | **Recommended.** All existing commands (hierarchy, inspector, gizmos, picking, undo, copy/paste, frame selected) work on the stage with no change, because they already mean "the loaded scene". The parked scene cannot be picked, selected or edited because no command can see it. The stage cannot reach the scene file because the scene save paths are refused while it is open (section 6). |

C needs: (1) `SceneManager::SwapLoadedScene(unique_ptr<Scene>) -> unique_ptr<Scene>`: replaces the loaded scene and returns the old one, without destroy callbacks, keeping stored scene data and not creating a stored entry (the stage is never `AddScene`d; a stored copy would be a second route into the scene list). (2) a host-side stage record. (3) a render overload (section 7).

**Host state** (`EditorServer`): `struct PrefabStage { Math::UUID prefabUuid; std::string path; std::string name; Mode mode; std::unique_ptr<Scene> stageScene (or loaded in SceneManager while open); std::unique_ptr<Scene> parkedScene; std::unique_ptr<EditHistory> history; std::unique_ptr<EditHistory> parkedHistory; revision and savedRevision; saved file stamp (mtime + size + sha) ; EditorCameraState sceneCamera, perModeCamera[2]; parkedOpenScenePath, parkedRevision; anchor (entity id in the parked scene, optional); warnings; }`. At most one stage. `GetHistory()` returns the stage's history while open; `ObserveScene()` (the generic "the loaded scene changed under me" detector) must be taught the stage swap explicitly (it would otherwise treat it as another scene being loaded and drop the open scene's file bookkeeping).

**What goes in the stage scene.** Exactly the prefab's tree and nothing else: no helper GameObjects. The default light and the grid are **render-only**: the default light is injected into the scene lighting data by the stage render path when Isolated and the stage has no light of its own (section 7); the grid is the existing viewport grid. Because no helper objects exist, the save path serializes the stage's single root with no exclusion list to get wrong, and the hierarchy panel shows only the prefab.

**Building the stage tree.** Deserialize the prefab file's `rootObject` into the stage scene **keeping the file's own UUIDs** (not `InstantiatePrefab`, which renumbers). Reasons: a Save produces a minimal diff (no UUID churn in version control), and references inside the prefab stay as saved. References to anything outside the prefab are null (they resolve against the stage scene only) and **will be dropped on save**: report them as a stage warning at open ("N references to objects outside the prefab were cleared"). UUIDs in the stage can equal UUIDs in the parked scene (the object the prefab was created from has the same ids); lookups are per scene, so there is no clash, but the frontend must clear its selection on a swap (section 8).

## 3. Commands and events (protocol 1.26.0)

Hello capability `prefabStage`; capability `prefabStageContext` only when the connected renderer supports the dim state (section 7), so the engine PR can ship before the renderer hook without a broken mode.

| Command | Class | Meaning |
|---|---|---|
| `OpenPrefabStage {uuidOrPath, mode, anchorEntityId?}` | Write | Parks the open scene, loads the stage, swaps history, camera, revision. `mode` is `isolated` or `context`. `anchorEntityId`: an instance in the parked scene whose `prefabReference` is this prefab (context placement); absent: the first instance found, else none (context with no instance: the prefab is shown at its own saved position, the response says `anchor: none`). Refused when a stage is already open (use Close first), when the host is a play host or read-only, when the asset is not a `Prefab`, unreadable or over the load caps. Answers `PrefabStageInfo`. |
| `SetPrefabStageMode {mode}` | Write | Switches Isolated/Context without touching the stage scene or its undo. Remembers a camera per mode. Pushes the event. |
| `SavePrefabStage {}` | Write | Writes the stage root to the `.prefab` (section 5). Marks the stage saved. Not undoable (as `ApplyPrefab`). |
| `ClosePrefabStage {discard}` | Write | Without `discard` and with unsaved changes: Error `unsavedChanges` (the frontend asks Save / Discard / Cancel, then calls Save then Close, or Close with `discard`). Restores the parked scene, history, camera, selection hint. |
| `GetPrefabStage {}` | Read | `PrefabStageInfo` or `{open:false}`. |

`PrefabStageInfo`: `{open, prefabUuid, path, name, mode, dirty, revision, savedRevision, anchorEntityId?, contextAvailable (an instance exists in the parked scene), sceneName, scenePath, warnings[], externalChange}`.

Event `prefabStageChanged` (same payload) is pushed on open, close, mode switch, save, dirty change, an external change of the file, and orphaning. Opening and closing also push `sceneChanged {full:true}` and `historyChanged` so existing client code refetches the hierarchy, the history and the frame, with no stage-specific handling for the common case. `EntityCreated`/`sceneChanged` ids while the stage is open are the stage's.

Role and policy: viewer refused (`forbidden`), `--read-only` host refused (`readOnly`), a play host refuses all five (they write or swap); not offered to MCP in v1 (MCP is a later bridge PR; `GetPrefabStage` read could be, with the bridge sync) and not to Lua extensions: extension `Editor.Scene` writes are **refused while a stage is open** (code `stageOpen`), reads see the stage.

## 4. Undo

A separate `EditHistory` instance per stage. It is created empty at open and dropped at close; the parked history is preserved and comes back at close (a prefab edit is never an undo step of the scene, and closing a stage cannot undo scene work). Ops address by UUID against "the loaded scene", which is the stage while open, and `RestoreSceneSnapshot` snapshots the loaded scene, so no op code changes; only `GetHistory()` and `PushHistoryChanged` follow the active history. The dirty flag is `revision != savedRevision` with the same mechanics as the scene (saving the file sets `savedRevision`; undo back to the saved revision is clean). Mode switch is not an edit.

## 5. Save and the prefab file

- Save serializes the **stage root only**: `Prefab::Serialize` form `{name, rootObject}`, through the same code `ApplyPrefab` uses to write the file (one writer; the file is not writable through `WriteTextAsset`). Differences from `ApplyPrefab`: the source is the stage root, not a scene instance, so the root's `prefabReference` is **cleared in the output** (a prefab file's root must not link to itself; verify what `CreatePrefab`'s serialization does today and keep one rule), and the prefab's `name` is kept.
- Root transform: in Isolated the root's saved transform is what the user sees and edits. In Context the root is **displayed at the anchor instance's world transform** (so it appears where the instance is) and the root's own transform is **locked** (gizmo and inspector transform fields refuse writes with a clear message); on Save the root TRS written is the one the file had, or the last Isolated edit. Switching modes swaps the root between the two placements without recording an edit.
- Cycle guard: Save is refused (Error `prefabCycle`) when any object under the root has `prefabReference` equal to the prefab's own UUID.
- After Save the host does nothing to scene instances (v1: instances are copies with a link, no propagation; roadmap 10). It does bump the asset's loaded cache so the next `InstantiatePrefab`/`Prefab.Instantiate` sees the new data, and the normal asset watcher pushes `assetsChanged {modified}`.
- Write failure (permissions, disk) is an Error and the stage stays dirty.

## 6. Keeping the stage out of the scene, and what is refused while it is open

The stage objects never touch the scene file because the scene is not the loaded scene's owner any more and the paths that serialize "the loaded scene" are refused:

- Refused with `stageOpen`: `SaveSceneToFile`, `SaveScene`, `OpenScene`, `NewScene`, `LoadScene`, `CreateScene`, `DeleteScene` (of the parked scene's file), `RestoreAutosave`, `DiscardAutosave`, starting a play session, `CreatePrefab`, `InstantiatePrefab` (no nested instances in a stage, see below), `ApplyPrefab`, `UnpackPrefab`, `SetStartupScene` stays allowed (project file, no scene).
- **Autosave is suspended** while the stage is open (`FlushAutosave` returns early; `NoteEditStepEnded` does not mark the scene due). The parked scene keeps its pending autosave state. Without this, autosave would write the prefab's tree as the scene's autosave. An optional stage autosave to `.n2/autosave/prefab-<uuid>.json` could come later.
- Everything else (asset commands, `SerializeEntities`/`InstantiateEntities` copy-paste within or between scene and stage, project settings, `SetEditorCamera`, `RenderFrameIfChanged`) works.
- The host shutting down or the client dropping with a stage open: the stage is discarded (not saved) and the parked scene is restored, so shutdown paths free the right objects. A reconnecting client calls `GetPrefabStage` and resumes (the stage lives in the host, not the client), or closes it.

**Play mode while a stage is open: refuse.** Starting play snapshots the loaded scene, which would be the stage. Recommendation: the host refuses with `stageOpen`; the frontend's Play button, on seeing a stage, offers "Close the prefab stage to play" (Save/Discard prompt, then Close, then Play). Alternative (auto-leave) hides a state change inside Play; refusing is explicit and cheap.

**Nested prefabs (out of scope in v1).** A prefab file may contain objects with `prefabReference` to another prefab (created from a hierarchy that had instances). The stage treats them as **plain objects**: they are loaded and shown, their `prefabReference` values are kept untouched in the file on Save, no badge or "Open prefab" for them, a warning at open ("contains N linked objects, edited as plain objects"). Instantiating a prefab inside a stage is refused (`stageOpen` row above), which keeps a cycle impossible to create; the cycle guard covers hand-made files.

## 7. Rendering: showing the stage, and dimming the rest

**Isolated.** The loaded scene is the stage, so `RenderEditorView` draws it unchanged. Differences: lighting. `Application::RenderEditorFrame(camera, StageView)` (new overload) builds `SceneLightingData` from the stage scene, and when `StageView.defaultLight` is set and the scene has no light, appends one directional light (fixed direction, neutral colour, modest ambient). The clear colour/ambient/skybox come from `RenderSettings` of the stage scene (a bare scene has defaults: verify `RenderSettings.cpp`; Isolated should use a fixed neutral background and not inherit the parked scene's sky).

**Context.** Two draws into one frame, in the same `BeginFrame`/`EndFrame`, sharing depth:
1. Pass 1: the **parked scene** with `dim` > 0 (the backdrop). Its lights supply `SceneLightingData` for **both** passes so the prefab is lit as it would be in the scene. The anchor instance (if any) is **hidden** in pass 1 by a draw-skip of that subtree (render-only: a set of GameObject pointers in `StageView`, checked by the scene traversal) so the prefab is not drawn twice.
2. Pass 2: the stage scene, `dim = 0`, with the root placed at the anchor's world transform by a render-and-pick **placement matrix** (the stage root's world transform is set to the anchor's at open/switch, as the section 5 lock describes: the stage objects really sit there in the stage scene, so picking and gizmos need no matrix tricks).
Screen-space UI is not drawn in either (the editor view already passes `screenSpaceUI=false`); world canvases of the backdrop are drawn dimmed.

**Dim hook (render-only, travels with the draw).** Add `float dim = 0.0f` to `RenderState` (0 none; 1 is fully the dim colour; the shader computes `rgb = mix(rgb, luma*dimColour, dim)`-style desaturate-and-darken, constant `dimColour`, so dimmed content stays readable but clearly inactive). It must be part of the per-draw state, not a renderer-global setter, because of the reordering rule above. How each renderable gets it: `RenderState` is created inside each renderable (`MeshRenderer::Render` uses `RenderState::Opaque()/Transparent()`), so the scene traversal sets a **frame-scoped modifier on the renderer** (`IRenderer::SetStageDim(float)`), which every backend **reads inside its DrawMesh call, at call time, and stores into the draw record** (the same place it already stores the passed state). That keeps the "travels with the draw" property without editing every renderable (meshes, text, particles, soft bodies, UI all end in `DrawMesh`/`DrawObjects`), and `SetStageDim(0)` after pass 1. Layers were considered and rejected: layers are user data with collision meaning, and a per-object tint would need every renderable edited.

Per backend (what each needs; no code in this proposal):

| Backend | Work |
|---|---|
| **OpenGL** (`renderer/src/opengl/OpenGLRenderer.cpp`, shaders) | one `u_stageDim` uniform in the fragment shaders (lit, unlit, text, particle variants), set per `DrawMesh` from the stored state; blended draws dim the colour before blending. |
| **Vulkan** (`renderer/src/vulkan/VulkanDraw.cpp`, `renderer/shaders/vulkan`) | dim in the per-draw data (the material set-1 dynamic-offset snapshot, or a push constant if the layout has room), the fragment shaders edited and SPIR-V rebuilt; no new pipelines (it is a value, not a pipeline key). Validation-layer clean is part of its test. |
| **WebGPU** (`renderer/src/webgpu/WebGpuDraw.cpp`, `renderer/shaders/webgpu`) | the same field in the per-draw uniform, WGSL edited, `scripts/validate-wgsl.sh` and the negative cases in `scripts/wgsl-negative` kept green; uniform buffer layout/alignment re-checked (the 16-byte rule). |
| **Software** (`renderer/include/renderer/software`, `SWShader`) | the CPU shader applies the same mix; `DrawOrder` carries the value (it reorders draws, which is the reason for the rule). Pixel tests are possible here and are the reference for the others. |

The editor host normally runs OpenGL or software headless (CI), so the **software and OpenGL pixel/golden tests carry the proof**; Vulkan and WebGPU get a draw-record test plus a smoke render and shader validation.

**Not pickable / not editable.** Free from architecture C: picking (`ScenePicking`) and every command resolve against the loaded scene, which is the stage. The backdrop is only ever drawn. `PickEntity` hits only stage objects (a click on dimmed geometry hits nothing and the frontend can show "Not part of the prefab"). Marquee/box select the same.

## 8. Entity ids, selection, camera, frontend

- Entity ids in the stage are the prefab file's UUIDs (section 2); they can coincide with scene ids. The frontend must treat open/close as a context change: **store the scene selection and expansion before `OpenPrefabStage`, clear them, restore them after `ClosePrefabStage`** (restoring ids that still exist; an object deleted meanwhile is impossible since the scene is parked and unchanged). `sceneChanged {full}` already makes the hierarchy refetch.
- Camera: one `_editorCamera`; on open save it as `sceneCamera`, on close restore it. Each mode has its own remembered camera for the stage. First Isolated view: framed on the stage root's bounds (existing frame-selected maths). First Context view: the scene camera unchanged, so the prefab appears in place (if an anchor exists the host may frame it; optional). Orthographic/perspective choice follows the camera state.
- Frontend (frontend repo, outline): double-click a `.prefab` row (and an inspector "Open" button on a prefab instance, `Prefab: X`) calls `OpenPrefabStage` with mode from the last-used (default Isolated; Context when opened from an instance, with `anchorEntityId`); a **stage toolbar** over the viewport: breadcrumb `Scene name > Prefab name` (clicking the scene name = close with prompt), Isolated/In Scene toggle (disabled with a tooltip when `contextAvailable` is false... Context with no instance still works, the prefab is shown at its own position), **Save**, **Close**, a dirty dot, a banner for warnings and for `externalChange`; unsaved-changes dialog (Save / Discard / Cancel) on Close, on window close, on opening another stage and on Play; the hierarchy panel shows the stage tree with a header "Prefab: X"; the dim/not-selectable behaviour needs no frontend work beyond the toolbar; disable scene-only actions (Save Scene, New/Open Scene, Play, Create/Apply/Unpack Prefab) with the reason; root transform fields read-only in Context; reconnect: call `GetPrefabStage` after Hello and show the toolbar if open. Capabilities `prefabStage` / `prefabStageContext` gate the feature.

## 9. assetsChanged while a stage is open

- The stage keeps the prefab's UUID and the file stamp (mtime, size, hash) it loaded or last saved. On `assetsChanged` (the host's own watcher) the host compares: **unchanged** (it was our Save): nothing. **Modified by something else**: set `externalChange` in the info and the event; the frontend offers "Reload (discard my changes)" (= Close discard and reopen) or "Keep" (a later Save overwrites it, the dialog says so). **Deleted or moved**: with stable UUIDs the host resolves the UUID to the new path (a move) and updates `path`; if the UUID no longer exists (deleted) the stage is flagged `orphaned`: Save is refused until the user chooses Save As (a path prompt, `SavePrefabStage {path}` as a small extension, or Discard); this is v1-optional: orphaned = Close discard offered, Save refused.
- Assets used by the stage (materials, textures, scripts) reload through the normal paths; `assetsChanged` also keeps the Assets panel live as today.
- The parked scene's assets are unaffected; if the **scene file** changes on disk while parked, nothing is done until Close, when the normal scene-changed handling applies.

## 10. Test plan

- Host (headless, software renderer, in `editor_server` tests): open/close round trip leaves the scene bytes, revision, history, camera and autosave state **identical**; stage objects absent from `Scene::Serialize` of the scene before/after; every command in the refused list returns `stageOpen`; Save writes the root only, clears the self `prefabReference`, keeps names, writes no UUID churn on an unmodified save (byte-equal file for an untouched prefab: the key test of keeping the file's UUIDs); undo in the stage does not touch the scene history and vice versa; dirty/clean through undo; unsaved Close errors, `discard` closes; external modification sets `externalChange`; delete/move handled; nested linked objects survive a round trip; cycle guard; caps; read-only/viewer/play host refusals; extension write refused; client drop with a stage open restores the scene; autosave not written while open and back afterwards; mode switch preserves the stage and is not an undo step; root transform lock in Context; protocol golden and test vectors at 1.26.0; Hello capabilities.
- Rendering: software renderer golden images (backdrop dimmed, stage lit as in the scene, anchor instance hidden, default light in Isolated only); OpenGL golden image where the CI has a context; Vulkan: draw-record test + validation-clean smoke; WebGPU: uniform layout test + `validate-wgsl.sh`; a **reorder test** on the software renderer (dim stays with its draw when draws are reordered).
- Picking: with a stage open, `PickEntity` over the dimmed backdrop returns nothing.
- Frontend: Node + DOM tests for the toolbar, the prompt flows (Save/Discard/Cancel on Close, Play, scene open), selection save/restore, capability gating, reconnect resume, banner for `externalChange`; live check in the app for double-click, mode switch and the dimmed backdrop.
- Docs in the same PRs (project rule): `docs/editor.html`, `docs/serialization.html` (prefab stage), `docs/scenes.html` if needed, protocol.json/test vectors; `docs/mcp.html` is the user's separate task.

## 11. Size, risk, PR order

Size: engine + host **L** (SceneManager swap, stage record, 5 commands + event, refusal list, history/autosave/camera handling, render overload with backdrop pass and default light); renderer hook **M-L** (one state field, four backends, shaders, golden tests); frontend **L** (toolbar, prompts, panel gating, selection handling).

Risks: (1) the refusal list: any missed scene-serializing path would write the stage into the scene (mitigated by the "scene is parked, loaded scene is the stage" test over every Write command, run as a table-driven test that calls each scene-saving command with a stage open); (2) `ObserveScene`/`_openScene` bookkeeping and `SceneManager` internals assume a loaded scene is a scene in the list; (3) renderer shader changes in four backends (WebGPU alignment, Vulkan SPIR-V rebuild) are the highest-friction part; (4) Context root placement/lock is the subtlest UX rule (decision 3); (5) a crash with a stage open loses the unsaved stage (no stage autosave in v1).

PR order:
1. **Engine + protocol 1.26.0**: `SceneManager::SwapLoadedScene`, the stage record, commands and event, refusals, history/autosave/camera swap, render overload (draws the backdrop undimmed until the hook lands; `prefabStageContext` capability off), docs, host tests. The frontend can already do Isolated against it.
2. **Renderer hook**: `RenderState`/`SetStageDim`, the four backends, goldens; flips `prefabStageContext` on (the capability is per the connected renderer, so a backend without it still hides Context).
3. **Frontend**: protocol sync, stage toolbar, prompts, gating (Isolated first behind the capability, Context when the flag is on; both ship in the first **release**).
4. Docs-only ledger and MCP entries last.

## DECISIONS for the user

1. **Stage architecture**: swap the loaded scene and park the open one (C), over a temporary subtree in the scene (A) or a second scene with new plumbing (B). Recommendation: C.
2. **Prefab UUIDs in the stage**: keep the file's own UUIDs (minimal diff on Save), not fresh ones. Recommendation: keep.
3. **Root transform in Context**: shown at the instance's place, transform locked, saved root TRS unchanged (Isolated edits it). Alternative: let Context edits write the root. Recommendation: locked in Context.
4. **Play while a stage is open**: refuse with a prompt in the Play button. Recommendation: refuse.
5. **Extension writes while a stage is open**: refused. Recommendation: refuse writes, allow reads.
6. **External references in a prefab**: cleared on open and on Save, with a warning. Recommendation: clear and warn.
7. **Nested prefabs**: edited as plain objects, links preserved in the file, instantiating inside a stage refused. Recommendation: as written.
8. **Orphaned (deleted) prefab**: Save refused, Close discard offered; Save As later. Recommendation: no Save As in v1.
9. **Autosave for the stage**: none in v1 (scene autosave suspended). Recommendation: none, revisit with usage.
10. **Dim style**: desaturate-and-darken, fixed strength, in all four backends via a per-draw `dim`. Recommendation: yes; a user-set strength later.
11. **Protocol number**: 1.26.0 after 1.25.0 (references). Recommendation: confirm; renumber if merge order flips.
