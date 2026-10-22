# Lua editor extensions ("editor scripts like Unity") (design proposal, 2026-10-15)

Tags: [V] verified by reading N2Engine `origin/master` at `a1aa1005` and N2Engine-Electron-Frontend `origin/master` at `d002d9f` (both checkouts were identical to origin and clean); [M] assumption or general knowledge, not checked in this code. N2Engine paths are relative to N2Engine/; `[FE]` marks a frontend path. Design only: nothing was built, run or changed.

Inputs it must stay consistent with (both approved by the user on 2026-10-15):
- `proposals/security-design-lua-editmode-and-preauth-slots.md` Part 1, **option B**: on every non-`--play` host, scene scripts are not executed in the shared state; only declaration-only evaluation runs, in a throwaway state with no engine bindings (PRs L1, L2), plus S4 budgets (L3) for hosts that really run scripts.
- `proposals/editor-host-security-audit-2026-10-15.md`, in particular **F3** (the editor role's Lua power) and its fix "make running project Lua on a remote host a separate right", F9 (viewers read the whole log) and F10 (paths in errors).

---

## 0. Summary

**Today there is no editor-extension infrastructure** [V]: Lua is runtime scripting only (`LuaComponent`, `LuaRuntime`); `engine/src/scripting/bindings/engine-api.lua` (2664 lines) never mentions the editor; no docs page covers editor scripts, menu items, custom inspectors, editor windows or "execute in edit mode"; the frontend has no extension point. There are, however, solid pieces to build on (section 1.2).

**Recommendation: a hybrid, host-side design.**
- Extension code is **Lua that runs on the host**, in a **dedicated, per-extension sandboxed state** that is separate from both the game's global state and option B's declaration evaluator. It has an `Editor` API and **no engine bindings**.
- Its **UI is data**: menu items, parameter forms and inspector sections are declared as descriptors that the Electron app renders with its existing widgets. **No project code ever runs in the Electron app.**
- Every scene change an extension makes goes **through the existing command layer** (the socket-less `ExecuteCommand` path). Validation, role and read-only gates, audit lines, `sceneChanged`/`historyChanged` events, the dirty flag and autosave are therefore reused, and one invocation is **one undo step** (one edit group).
- Extensions live in **`<project>/editor/**/*.lua`, outside `assets/`**. Protocol writes (`WriteTextAsset`), `require`, the play host and any future game build cannot reach them by construction. An editor-role user cannot plant extension code.
- They are **opt-in twice**: the host loads them only with `--editor-extensions` (default OFF on every host), and the Electron launcher passes that flag for a local project only after a per-project trust prompt. They load only at start or on an explicit admin reload, **never as a side effect of loading a scene**.
- **First slice:** host-side menu commands that run over the selection with undo, plus a declared parameter form, a Tools menu and output in the console. About 4 engine PRs and 1 frontend PR.

---

## 1. What exists today

### 1.1 What is missing [V]
- No `Editor` table or editor-only binding: `grep -i editor engine/src/scripting/bindings/engine-api.lua` finds nothing; `LuaRuntime::RegisterBindings` (`engine/src/scripting/LuaRuntime.cpp:102-125`) binds only runtime systems.
- No "execute in edit mode" concept: `grep -ri 'ExecuteInEditMode\|RunInEditMode\|EditorOnly'` over `engine/` and `docs/` finds nothing. The edit host never ticks (`editor-server/src/Host.cpp:538-558` only calls `ProcessCommands`), and option B removes even top-level chunk execution there.
- No debug or gizmo drawing from Lua: `Debug.DrawLine`/`DrawRay` are commented out (`engine/src/scripting/bindings/LuaDebugBindings.cpp:11-30`). Gizmos are drawn and hit-tested **on the client** ([FE] `src/renderer/viewport-gizmo.ts:1-11`).
- No game build or export step exists yet: the only executables are `N2EditorHost`, `lua_project`, `test_project` and the tests; a standalone `N2Player` is "later work (#6)" (`docs/editor.html:740`). "Build exclusion" therefore means: never loaded by a `--play` host, by `lua_project`, or by a future player.
- Frontend menus: the in-page menu type is flat, with no submenus ([FE] `src/renderer/ui.tsx:224-227`). The native application menu exists only on macOS and only for Undo/Redo ([FE] `src/main/app-menu.ts:1-28`); Windows and Linux use the in-page Edit menu ([FE] `src/renderer/editor.tsx:174-202`).
- `EditorServer::Capabilities()` returns an empty list (`editor-server/src/EditorServer.cpp:2025-2028`), so there is no feature flag yet.

### 1.2 What helps (reuse)
| Piece | Where [V] | How extensions use it |
|---|---|---|
| **Socket-less command execution** | `EditorServer::ExecuteCommand` (`EditorServer.cpp:2429-2470`) runs `ProcessCommand(-1, …)`; "handlers only build the response, which SendResponse records in `_response`". `ProcessCommand` applies the read-only gate, `RoleAllows`, the play-host deny list and the `audit:` line (`:2752-2795`) | `Editor.Scene.*` becomes "send a command internally as the invoking user": every existing validation and gate applies, and the extension can never exceed the invoker's role |
| **Undo/redo and edit groups** | `EditHistory` (`editor-server/include/editor-server/EditHistory.hpp`): `BeginGroup`/`EndGroup`/`CloseGroups`, nesting up to 16, `GroupEnd::{Committed,NoChange,Empty}`, a coalescing window, `MarkSaved`/`IsAtSavedState`. `RecordEdit` (`EditorServer.cpp:5906-5925`) records only for an edit-mode scene. `CloseEditGroups`/`NoteGroupWithoutChange` (`:5928-5958`). The handlers `HandleBeginEditGroup`/`HandleEndEditGroup` (`:6024-6070`) | One invocation = `BeginGroup(label)` … nested commands … `EndGroup()`: one Ctrl+Z, with the dirty flag and autosave as for any group |
| **Dirty flag** | `_sceneRevision`/`_savedRevision`, `EditResult.savedRevision`; the client's `sceneDirty` ([FE] `src/renderer/store.ts`, `hasUnsavedChanges`) | Nothing new: nested commands move the revision as usual |
| **Field schema system** | `FieldKind`/`FieldInfo` (`engine/include/engine/serialization/FieldInfo.hpp:28-97`) built by `RegisterMember(...).Range().Tooltip().AsColor()` (`ComponentSerializer.hpp:71-150`); `ValidateFieldValue`/`ClampFieldValue`/`ValidateFieldValues`; `ComponentRegistry::Describe` (`ComponentRegistry.hpp:100`); the wire `FieldSchema`/`ComponentSchema` jsonTypes (`editor-server/protocol/protocol.json`) | Form fields and inspector sections are **`FieldSchema` lists**, validated on the host with the same functions and rendered by the client's existing field editors |
| **Client field editors** | `FieldEditor(props: EditorProps)` with `{field, value, disabled, commit, invalid}`, independent of any entity ([FE] `src/renderer/inspector-editors.tsx:34-43`, `:916`); `checkValue`, `defaultFieldValue`, `numberLimits` ([FE] `src/renderer/inspector-fields.ts`) | The form dialog is a list of `FieldEditor`s over a local value object |
| **Lua declarations (`SerializableFields`)** | `LuaComponent::DescribeFields`/`ExtractSerializableFields` (`engine/src/scripting/LuaComponent.cpp:241-288`, `:699-820`); `GetLuaFields` (protocol 0x65); docs `docs/scripting-lua.html#fields` | The same declaration style (`{type=, default=}`) is used for form parameters, so authors learn one convention. Option B's evaluator shows that a separate, binding-free state is workable |
| **Lua sandbox** | Only `base`, `math`, `string`, `table`; no `dofile`/`loadfile`; text-only `load` (`LuaRuntime.cpp:40-71`); `require` through ResourceLoader | The extension state is built by the same (shared) sandbox builder as option B's evaluator (PR L1), with S4 budgets (L3) |
| **Asset watcher / hot reload** | `PollAssets`/`PollAssetsIfDue` (`EditorServer.cpp:5375-5420`, off in play mode), `ApplyAssetChanges` → `assetsChanged` event (`:5335-5350`), `ReloadLoadedAsset` (`:5299-5333`) | The pattern (a mtime poll plus an event) is copied for the `editor/` folder; `assetsChanged` feeds an `OnAssetsChanged` hook |
| **Event stream** | `PollEvents` (0x05) with `EditorEvent` kinds (`log`, `sceneChanged`, `assetsChanged`, `historyChanged`, …) | New kinds `extensionsChanged` and `extensionOutput` |
| **Classification table** | Every command is classified Read/Write/Admin (`EditorServer.cpp:2565-2645`). `IsReadCommand` is a default-deny allow list (`EditorServer.hpp:527-544`), and a test requires every command to be classified | New commands get an explicit class (section 6) |
| **Client write guard** | `readOnlyReason`/`canWrite`/`canEdit`/`hostRole` ([FE] `src/renderer/store.ts:230-255`); `followWriteGuard` ([FE] `src/renderer/write-guard.ts`); DOM tests `src/test-dom/read-only.test.tsx` | Tools menu items and form OK buttons follow `canEdit` |
| **Client edit groups** | `EditGroups` ([FE] `src/renderer/edit-groups.ts`), `edit-controller.ts` settles pending edits before undo | Before `InvokeMenuItem` the client settles pending edits and closes its own groups, as it does before Undo |

---

## 2. Constraints from the approved security work

1. **Option B stays true:** on an edit host, opening, loading, undoing, duplicating or hot-reloading a scene runs **no** project Lua with engine bindings. Extensions must not reintroduce that: they are **never** loaded or called from any scene path.
2. **Three distinct execution contexts, never mixed:**

| Context | Where | State | Bindings | Triggered by |
|---|---|---|---|---|
| **Run** | `--play` host, `lua_project`, future player | the global `sol::state` (`LuaRuntime`, `LuaRuntime.hpp:19`) | the full engine API | the game loop |
| **DeclarationsOnly** (option B) | every non-`--play` host | a throwaway state per script source, cached by hash | none (inert stubs, pure-Lua `Vector3`) | reading a LuaComponent's fields |
| **EditorExtension** (this proposal) | a non-`--play` host started with `--editor-extensions` | **one persistent state per extension file** | the `Editor` API only, with no engine usertypes | an explicit client command (`InvokeMenuItem`, …) or a declared hook, and only after the host loaded extensions at start or by admin reload |

3. **F3:** running project code on a host must be a **separate right**, visible and auditable, with CPU and memory budgets (the S4 mechanism).
4. **F9/F10:** extension output and errors must not widen what a viewer learns (no absolute paths, and sensitivity-tagged events once item 10 lands).

---

## 3. Q1: where does extension code run?

| | **H: host-only Lua, UI as code** | **F-JS: frontend JavaScript** | **F-Lua: frontend Lua (fengari or wasmoon)** | **Hybrid (recommended): host Lua plus declarative UI** |
|---|---|---|---|---|
| Access to the scene and engine | direct, on the host | only through the protocol, and the frontend may be remote from the host | as F-JS | direct (through the command layer) |
| Remote hosts (the author's machine is not the host) | works: code ships with the project on the host | code must come from the project, which lives on the host: a new "fetch extension source" path, then run on every client | as F-JS | works |
| Security surface | host sandbox (already exists, plus S4 budgets) | **arbitrary project JS inside Electron**: the CSP is `script-src 'self'` ([FE] `src/index.html:5`), so it would need `eval`/`new Function` or an extra renderer, and an XSS-like surface for A4 (a malicious host) | an interpreter in the renderer (fengari is pure JS [M]; wasmoon needs `'wasm-unsafe-eval'` in the CSP [M]), and still project code steering the UI | **no project code in the app at all**; the client renders validated data |
| UI richness | the host cannot draw the client's UI | full | full | bounded by the descriptor vocabulary (forms, menus, sections, buttons, output) |
| Undo, roles, audit | reuse | the client sends ordinary commands (reuses gates), but logic and undo grouping are the client's | as F-JS | reuse, enforced on the host |
| Consistency with option B and F3 | a separate, gated host context | moves the trust problem into the editor app, and every collaborator's app runs the project's code | as F-JS | a separate, gated host context |

**Recommendation: Hybrid.**
- Code runs where the scene is (the host), under the host's sandbox, budgets and role gates.
- The Electron app only renders descriptors (strings, numbers, `FieldSchema`s) through its existing text-only widgets. That keeps the audit's A4 posture ("no HTML sinks, text-only rendering") intact.
- The cost is a fixed UI vocabulary, which is accepted. Richer panels come later as more descriptor types (section 9), not as client code.

---

## 4. Q2: the Lua API surface

### 4.1 Shape of an extension file
Each file under `<project>/editor/` is one extension and returns a table. Declarations are data, and behaviour is functions:

```lua
-- <project>/editor/AlignTools.lua
return Editor.Extension {
  name = "Align Tools",
  apiVersion = 1,
  capabilities = { "scene.read", "scene.write" },   -- what the host grants this state
  menu = {
    {
      id = "align.snapToGrid",
      path = "Tools/Align/Snap selection to grid",
      enabledWhen = { selection = "nonEmpty" },        -- evaluated by the client: no Lua per menu open
      confirm = nil,                                   -- or "Snap 40 objects?"
      params = {                                       -- a form, same convention as SerializableFields
        grid = { type = "number", default = 1.0, min = 0.01, max = 100, tooltip = "Grid size in metres" },
        snapY = { type = "bool", default = false },
      },
      run = function(ctx, p)
        for _, id in ipairs(ctx.selection) do
          local t = Editor.Scene.GetLocalTransform(id)
          local s = function(v) return math.floor(v / p.grid + 0.5) * p.grid end
          t.position = { x = s(t.position.x), y = p.snapY and s(t.position.y) or t.position.y, z = s(t.position.z) }
          Editor.Scene.SetLocalTransform(id, t)
        end
        return { message = ("Snapped %d objects"):format(#ctx.selection) }
      end,
    },
  },
}
```

### 4.2 The `Editor` namespace (v1 = API version 1)
Values are **plain Lua tables and UUID strings** (the JSON the protocol already uses), never engine usertypes. That makes the state memory-safe by construction: there are no dangling `GameObject` userdata (the audit's "could not verify" item).

| Area | Functions | Implemented as [design] | Capability |
|---|---|---|---|
| `Editor.Extension{}` | the declaration constructor (validates the shape) | pure Lua or C++ check | none |
| `Editor.Scene` read | `GetHierarchy()`, `GetEntity(id)`, `GetComponent(componentId)`, `GetLocalTransform(id)`, `FindByName(name)`, `GetComponentTypes()` | internal `GetHierarchy`/`GetEntity`/`GetComponent`/`GetComponentTypes`, decoded to tables | `scene.read` |
| `Editor.Scene` write | `CreateEntity{name, parent, components}`, `Destroy(id)`, `Duplicate(id)`, `SetParent(id, parent, index)`, `SetProperties(id, {name, tag, layer, active})`, `SetLocalTransform(id, t)`, `AddComponent(id, type, values)`, `RemoveComponent(componentId)`, `SetFields(componentId, values)` | internal `CreateEntityEx`, `DestroyEntity`, `DuplicateEntity`, `SetEntityParent`, `SetEntityProperties`, `SetLocalTransform`, `AddComponent`, `RemoveComponent`, `SetComponentFields` | `scene.write` |
| `Editor.Selection` | `ctx.selection` (the client's selection, UUIDs, sent with the invocation); the result's `select = {…}` asks the client to change it | data in and out; the selection is a client concept ([FE] `src/renderer/viewport-selection.ts`) | none |
| `Editor.Assets` | `List(folder)`, `ReadText(path)`; later `WriteText(path, text)`, `CreateFolder(path)` | internal `ListAssets`, `ReadTextAsset`, `WriteTextAsset`, `CreateFolder` (`res://` only, the same resolver) | `assets.read`, `assets.write` |
| `Editor.Log` | `Info`, `Warn`, `Error` | an `extensionOutput` event plus a log line tagged with the extension | none |
| `Editor.Form` | **declared**, not called: `params` on a menu item or button | see 4.4 | none |
| `Editor.Inspector.Register` | `(componentType, { sections = { {label, fields = {...}, buttons = {{id, label, params, run}}} } })` | static descriptors plus button callbacks | `scene.read` (and `scene.write` for buttons that edit) |
| Hooks | `OnSceneOpened(info)`, `OnSceneSaved(info)`, `OnBeforePlay(info)` (validation only), `OnAssetsChanged(added, removed, modified)` | called by the host at those points, budgeted, see 4.5 | per hook |
| Math helpers | pure-Lua `Vector3`, `Quaternion.FromEuler`, … (the same pure-Lua stubs as option B's evaluator, but real) | Lua prelude | none |

Not in v1: anything that blocks waiting for the user (a Lua `Dialog.Show()` that returns a value). The host's main thread must not wait on a client. Forms are therefore declared up front, and the client collects the values **before** the invocation (4.4).

### 4.3 Undo, redo and the dirty flag
For `InvokeMenuItem` (and inspector buttons, and write hooks):
1. The client first **settles** its pending inspector edits and **closes its own groups** (the existing `edit-controller.ts` pre-undo sequence), so the extension's step is not merged into a gesture.
2. The host refuses with an Error if a client group is still open (`_history.InGroup()`). That keeps the step boundaries clear (decision D9).
3. The host calls `_history.BeginGroup(label)` with the label `"<item title>"` (cut to 100 bytes like `HandleBeginEditGroup`), then runs `run(ctx, params)`.
4. Each `Editor.Scene` write is an **internal dispatch** of the matching command as the invoking identity. Its handler calls `RecordEdit`, which adds the op to the open group (`Recorded::InGroup`), moves `_sceneRevision` and queues `sceneChanged`.
5. On success: `EndGroup()`. `Committed` gives one undo step with `historyChanged`; `NoChange` (writes that restored the original values) gives no step, and `NoteGroupWithoutChange` keeps the saved state, exactly as a cancelled gizmo drag does.
6. On a Lua error, a budget trip or a nested command's Error: `EndGroup()`, then, if it `Committed`, **undo that step at once** and drop it from the redo stack, so a failed invocation is atomic (decision D8). The response is `ErrorCoded extensionFailed` with the Lua message, with host paths stripped (F10).
7. The dirty flag needs nothing new: the revision moved, and `savedRevision` is unchanged, so `sceneDirty` turns on. Undoing the step back to the saved state clears it, as today. Autosave is flushed once after the command (`ExecuteCommand` already does it, `EditorServer.cpp:2442-2451`).

**Internal dispatch** [design]: do **not** call the public `ExecuteCommand` re-entrantly. It clears `_response` on entry and flushes the autosave on exit [V: `:2431`, `:2442-2451`]. Instead add `DispatchNested(commandType, payload) → response bytes`, which:
- saves and restores `_response`;
- goes through `ProcessCommand(-1, …)`, so the read-only gate, `RoleAllows`, the play-host deny list and the audit line all apply;
- suffixes the audit line with `via=<extension>`;
- has its own cap on nested commands per invocation (D7).

Commands that make no sense from inside an extension are refused by an explicit deny list: `Undo`, `Redo`, `Begin`/`EndEditGroup`, `OpenScene`, `NewScene`, `LoadScene`, `RestoreAutosave`, `Shutdown`, `Hello`, the play commands, and the extension commands themselves (no recursion).

### 4.4 Forms (parameter dialogs)
- A form is a list of `FieldSchema` (protocol.json `FieldSchema`: `name`, `displayName`, `kind`, `typeName`, `enumOptions`, `assetType`, `min`/`max`, `tooltip`, `hidden`, `readOnly`) plus defaults.
- The host builds it from the Lua `params` table with the **same kind rules as `GetLuaFields`**: `type = "number"|"int"|"string"|"bool"|"Vector3"|"GameObject"|<component type>|"enum"` with `options`, and an asset type for `AssetRef`.
- The client renders it with `FieldEditor` rows in a modal (a new `FormDialog` beside `PromptDialog`/`ConfirmDialog`, [FE] `src/renderer/ui.tsx:124-220`) and sends the values with `InvokeMenuItem`.
- The host re-validates them with `ValidateFieldValue` and `ClampFieldValue` before calling Lua, so Lua always receives well-formed values.
- Later (deferred): a dynamic form, where Lua computes the defaults from the selection through `GetMenuItemForm(id, selection)`.

### 4.5 Hooks
| Hook | Host point [V] | v1 semantics |
|---|---|---|
| `OnSceneSaved(info)` | after `SaveSceneFile` succeeds (`EditorServer.cpp:6593`), `_history.MarkSaved()` (`:6641`) | read-only (`scene.read`): logging or validation warnings only; it cannot edit, or the save would immediately be dirty |
| `OnSceneOpened(info)` | after `LoadOpenedScene` | read-only |
| `OnBeforePlay(info)` | in `HandleWritePlaySnapshot` (`:4172`) before the snapshot is written | **validation only**: returns warnings (shown, never blocking by default). Blocking play is decision D10 |
| After play | **not available on the edit host**: play is a second process that the Electron main launches and stops ([FE] `src/main/play-session.ts:1-12`); the edit host does not learn when it ends. Deferred, needs a client notification command |  |
| `OnAssetsChanged(a,r,m)` | `ApplyAssetChanges` (`:5335`) | read-only in v1; "post-processors" that rewrite assets are deferred (section 9) |

Hooks run under a **smaller budget** than invocations, and **never** inside a scene load or deserialisation. They run after the triggering command finished its own work, as a separate budgeted call. A hook error is logged and counted towards the extension's fault flag; it never fails the save, open or snapshot.

---

## 5. Q3: discovery and lifecycle

### 5.1 Location: `<project>/editor/**/*.lua` (outside `assets/`)
A project root holds `project.n2proj`, `assets/`, `.import/` and `.n2/` [V: `engine/include/engine/io/ProjectFile.hpp:17-25`, `:149-153`]. Putting extensions in a sibling `editor/` folder means:
- **Unreachable from the protocol's write paths.** `WriteTextAsset`, `CreateScriptAsset` and `CreateFolder` are confined to `assets/` by `ResolveAssetPath` [V: audit section 4, "Asset paths"], so **an editor-role user cannot create or change an extension**. If extensions lived under `assets/Editor/`, an editor could write `.lua` there [V: `.lua` is a text-asset extension, `EditorServer.cpp:206-207`] and turn "content editing" into "code that runs under extension privileges": exactly F3.
- **Build exclusion by construction:** ResourceLoader, `require`, the `--play` host and `lua_project` resolve only under `assets/`. They cannot load an editor script, and a future player packages `assets/` only.
- Not listed by `ListAssets`, so a viewer does not see it.
- **The alternative** (`assets/Editor/**` or `*.editor.lua`, Unity-like) needs a new deny rule in `ResolveAssetPath` for non-admins, plus a `require` and play-host exclusion. That is more rules to keep right, so it is not recommended (decision D2).

Inside an extension, `require("x")` resolves only within `<project>/editor/` (its own resolver, the same part checks as `IsValidAssetName`). **`load` is removed** from extension states: an extension must not turn asset text that an editor wrote into code that runs with its capabilities.

### 5.2 Lifecycle
1. **Load:** at host start, if `--editor-extensions` is set and the host is not `--play`: walk `editor/` (skip symlinks, as `ListAssets` does). Each file at most 1 MiB, at most 64 files [M: limits are a proposal]. Create the state, run the chunk under the load budget, and validate the returned `Editor.Extension{}`.
2. **Registry:** the host keeps `{id = relative path, name, sha256, apiVersion, capabilities, state: loaded|failed|faulted|incompatible, lastError, menu, inspectors, hooks}`.
3. **Reload:**
   - `ReloadEditorExtensions` (Admin class) recreates every state.
   - Optionally, `--editor-extensions-watch` (local default) polls `editor/` like `PollAssetsIfDue` (mtime plus size, then a content hash) and reloads only the changed file.
   - Each reload pushes `extensionsChanged` with a new `uiRevision`.
   - States are discarded on reload. There is no persistence in v1 (deferred: `Editor.Settings`, kept in `.n2/editor-extensions/<id>.json`).
4. **Isolation:**
   - one `lua_State` per extension, so one extension cannot read or break another's globals;
   - each call is `pcall`ed;
   - **fault flag:** 3 failures (errors or budget trips) in 10 minutes [M] set `faulted`, and the extension's items show as disabled with the reason until a reload;
   - a broken file only marks itself `failed`: the others load, and the editor keeps working.
5. **Versioning:**
   - `apiVersion` is an integer, and the host supports a set (v1: `{1}`).
   - An unknown version gives `incompatible` with the reason.
   - Additive changes (new functions, new descriptor keys) keep `apiVersion`. Removing or changing something bumps it, and the host keeps the old version's shim for at least one minor protocol cycle.
   - `ServerInfo.capabilities` (currently empty, `EditorServer.cpp:2025`) gains `"editorExtensions"` when the feature is compiled in, and the extension list says whether it is enabled.

---

## 6. Q4: security and roles

### 6.1 Threat framing
Extension code is project code that runs on the host with the host's Lua sandbox and the invoker's command rights. The relevant attackers from the audit:
- **A3 (an editor)** must not be able to *author* code that runs (section 5.1 closes that by location), nor exceed their role through an extension (6.3).
- **A2 (a viewer)** must not be able to *run* it (Write class).
- **A malicious project** (an untrusted git clone opened locally) must not run code just by being opened (the trust prompt, 6.2).
- **The admin or operator** who installs extensions is trusted, as for the tokens file.

### 6.2 The opt-in layers (all must be on)
1. **Host flag `--editor-extensions`** (name: decision D3; the brief's `--allow-editor-scripts` is the same thing). **Default OFF on every host.**
   - The host cannot tell a trusted project from an untrusted one, so it never decides alone.
   - Refused with `--play` (a play host never loads them).
   - Refused with a non-loopback `--bind` unless a `--tokens-file` is in use, so roles exist (consistent with audit item 7, fail-closed bind).
   - Allowed with `--read-only`: extensions are listed, but every invocation is refused by the existing read-only gate, because `InvokeMenuItem` is Write class.
2. **Local hosts (Electron launcher):** the launcher builds the host's arguments ([FE] `src/main/host-launcher.ts:102-106`). It adds `--editor-extensions` **only** when the project has an `editor/` folder **and** the user said yes to a trust prompt: "This project contains N editor extensions (names, capabilities). They run code on this computer's editor host. Trust this project?"
   - The answer is remembered per project folder, in the same store as recent projects. Folder-level trust (like VS Code's workspace trust) or content-hash trust is decision D5.
   - "Not now" starts the host without the flag, and a banner offers to enable it (which restarts the host).
   - This is the brief's "default ON only for loopback/local", made explicit.
3. **Remote hosts:** the operator adds the flag on the command line or in the systemd unit. The extension files arrive by git or the deploy, never through the protocol (v1 has no command that writes `editor/`).

### 6.3 Roles and what each may trigger
| Action | viewer | editor | admin | Class |
|---|---|---|---|---|
| See the extension list, menu items, forms and inspector sections (disabled for viewers) | yes | yes | yes | Read |
| Invoke a menu item or inspector button (`InvokeMenuItem`, `InvokeInspectorAction`) | **no** | **yes** (recommended, D4) unless the item says `minRole = "admin"` | yes | Write |
| Reload extensions | no | no | yes | Admin |
| Enable or disable extensions on the host | operator only (flag) | | | n/a |

Why an editor may invoke (D4):
- The code is fixed and installed by an admin or operator (an editor cannot change it, 5.1).
- Every scene or asset change it makes is re-checked against **the editor's own** role through `ProcessCommand`.
- An editor therefore gains **CPU on the host** (bounded by the budgets) and nothing else. That is the F3 trade-off, made explicit and bounded.
- Hosts that want "content editors run no code" use `minRole = "admin"` per item, or a host flag `--editor-extensions-role admin` (D4 alternative).

Reads are pure data: `ListEditorExtensions` and `GetEditorUi` return descriptors that were captured at load time, and **run no Lua**. That keeps Read class honest ("changes nothing", and no code runs for a viewer).

### 6.4 Sandbox and budgets
- **State:** built by the **same sandbox builder** as option B's declaration evaluator (PR L1). One function, with a test that lists the state's globals.
  - Libraries: `base` (without `load`, `dofile`, `loadfile`, `collectgarbage`; `print` routes to `Editor.Log.Info`), `math`, `table`, `utf8`, and `string` **without the pattern functions** in v1 (`find`, `match`, `gmatch`, `gsub`). Their C-level CPU is not seen by the count hook, the same reasoning as option B. Allowing them with input-size caps is decision D6.
  - No `io`, `os`, `package` or `debug`; no engine usertypes.
- **Budgets** (the S4/L3 mechanism: count hook with "trip and keep failing", plus the allocator cap) [M: values to tune]:

| Budget | Load chunk | Invocation | Hook |
|---|---|---|---|
| VM instructions | 10 M | 200 M | 20 M |
| Wall clock (checked in the hook) | 1 s | 5 s | 0.5 s |
| Memory per state | 64 MiB (persistent) | | |
| Nested commands per invocation | n/a | 10,000 | 0 (read-only) |
| Request bytes per invocation | n/a | 64 MiB total | n/a |

  - A trip raises an uncatchable error (the hook keeps failing), the group is rolled back (4.3), and the fault counter increments.
  - The main thread is blocked for at most the wall budget. Commands are serial anyway, and the invoking client is the one waiting.
- **Audit:**
  - `audit: user=<u> role=<r> extension=<name>@<sha8> item=<id>` for every invocation, hook-run summaries at debug level, and the existing per-command audit line with ` via=<name>` for each nested write.
  - Load and reload log the list `name@sha8 capabilities`.
  - `extensionOutput` events carry the extension name. After audit item 10 they are tagged so that viewers do not see other users' extension output (F9).
- **Capabilities:**
  - declared in the file; the host **does not install** the functions of an ungranted capability into that state (absent, not just refused);
  - listed in the trust prompt and in `ListEditorExtensions`;
  - v1 set: `scene.read`, `scene.write`, `assets.read`, `assets.write`;
  - reserved for later: `project.write` (SetProjectSettings), `import.hooks`.

### 6.5 How this fits option B, explicitly
- No scene path (`FromJSON`, `LoadScene`, `OpenScene`, undo restore, `RestoreSubtree`, `DuplicateEntity`, hot reload of a `.lua` asset) creates, calls or reloads an extension state. Extension states are created only by host start (flag on) and `ReloadEditorExtensions`. They are called only by the extension commands and the four named hooks.
- `LuaRuntime::SetExecutionPolicy(DeclarationsOnly)` still governs the global state on an edit host. Extensions never use the global state, so they never "re-open" engine bindings on the edit host.
- Option B's decision 6 deferred a "scripts capability for editors". **This proposal is that capability, in a narrower form:** fixed, admin-installed code with declared capabilities, not "editors may run arbitrary Lua".
- Option B's decision 4 (no `--run-scripts-in-edit-mode` escape hatch) is unaffected. Extensions are not a way to run component scripts in edit mode (section 8).

---

## 7. Q5: protocol and frontend

### 7.1 New commands (protocol minor bump)
Ids from the free block `0x80-0x86` [V: used ids end at `0x72`, then `0x90-0x97`; `0xC0-0xFE` are reserved for events].

| Id | Command | Request | Response | Class | Notes |
|---|---|---|---|---|---|
| 0x80 | `ListEditorExtensions` | none | `EditorExtensions` (JSON: `enabled`, `uiRevision`, list of `{id, name, sha256, apiVersion, state, error?, capabilities, itemCount}`) | Read | no Lua runs; `error` text is path-stripped |
| 0x81 | `GetEditorUi` | none | `EditorUi` (JSON: `uiRevision`, `menuItems[{id, extension, path, title, enabledWhen, confirm?, minRole, form?: {fields: FieldSchema[], defaults}}]`, `inspectorSections[{componentType, extension, label, fields: FieldSchema[], buttons[{id, label, form?}]}]`) | Read | no Lua runs; captured at load |
| 0x82 | `InvokeMenuItem` | `itemId` string, `selection` JSON (UUID array, `ReadBoundedJson`, at most 4096 like `MaxBoundsEntityIds`), `params` JSON, `uiRevision` u32 | `ExtensionResult` (JSON: `message?`, `select?[]`, `warnings?[]`) plus the edit result (`revision`, `savedRevision`, `canUndo`, `canRedo`, `label`) | Write | refused on a stale `uiRevision`, with a client group open, or when faulted |
| 0x83 | `InvokeInspectorAction` | `componentId`, `actionId`, `params` JSON, `uiRevision` | `ExtensionResult` | Write | the button's `run(ctx, p)` gets `ctx.componentId`, `ctx.entityId` |
| 0x84 | `ReloadEditorExtensions` | none | `EditorExtensions` | **Admin** | reloads every state |
| (later) 0x85 | `GetMenuItemForm` | `itemId`, `selection` | a form | Write (runs Lua) | dynamic forms, deferred |
| (later) 0x86 | `NotifyPlayEnded` | none | Ok | Write | an "after play" hook, deferred |

- **Events** (new `EditorEvent` kinds; old clients ignore unknown kinds): `extensionsChanged {uiRevision}` and `extensionOutput {extension, level, message}`. They are also mirrored as ordinary log lines, so old clients' consoles show them.
- **Error codes** (`errorCodes`):
  - `extensionsDisabled`: the host was not started with the flag;
  - `extensionFailed`: a Lua error, a budget trip or a nested command's error, with the message;
  - `extensionFaulted`: disabled until reload.
  - A role refusal keeps `forbidden`.
- **Classification:** the five commands are added to the explicit table (`EditorServer.cpp:2565-2645`), so the "every command classified" test passes. `IsReadCommand` stays default-deny. `IsEditOnlyCommand` gains 0x82-0x84 (a play host refuses them).
- **Version:** minor bump. Part 2's S-4 also plans 1.13.0 (`busy`). Whichever merges first takes 1.13.0 and the other 1.14.0. Regenerate the clients (`editor-server/protocol/generators`), update `test-vectors.json`, and run the frontend's `npm run sync-protocol`.
- **ServerInfo.capabilities:** `"editorExtensions"` when compiled in. The client shows the Tools menu only when the capability is present **and** `ListEditorExtensions.enabled` is true.

### 7.2 Frontend changes (fits the existing architecture)
- **Store** ([FE] `src/renderer/store.ts`): `extensions` and `editorUi` signals, fetched after Hello and on `extensionsChanged` (in `editor-events.ts`, a new parsed kind). A computed `extensionsAvailable` for the toolbar.
- **Tools menu:** a toolbar button next to Edit (all platforms, in-page; [FE] `editor.tsx:174-202` pattern).
  - `MenuItem` is flat today ([FE] `ui.tsx:224-227`). Either add one level of submenus or show `path` groups as headers with separators. A pure function `buildToolsMenu(items, selection, role, canEdit)` is unit tested.
  - `enabledWhen` (`selection: any|nonEmpty|one|none`, `componentType`) and `minRole` are evaluated on the client, and the host enforces the role again.
  - Disabled with the reason when `!canEdit` (the write guard: play mode, read-only host, viewer).
- **Form dialog:** `FormDialog` in `ui.tsx` beside `PromptDialog`; rows are `FieldEditor`s over a local values object; `checkValue` per field; OK is disabled while a field is invalid.
- **Invocation flow** (a new pure module `extension-actions.ts`, unit tested with fakes like `edit-controller.ts`): settle pending edits, then `groups.closeAll()`, then `confirm?`, then the form, then `InvokeMenuItem`. It then applies `select`, shows `message`/`warnings` (status line or banner), and polls events, as after Undo.
- **Inspector sections** (stage E-X5): under a component of `componentType` in `inspector-panel.tsx`, render `label` and read-only `fields` (static or labelled values) and buttons. A button opens its form and then sends `InvokeInspectorAction`.
- **Output:** `extensionOutput` goes to the existing console ([FE] `console-store.ts`) with an "extension" source tag and a filter chip. There is no separate panel in v1.
- **Trust prompt and launcher:**
  - `project-session.ts`/`host-launcher.ts` checks `<project>/editor/*.lua` (main process, local only);
  - shows the prompt (a main-process dialog, since it gates process arguments);
  - stores the answer;
  - passes `--editor-extensions --editor-extensions-watch`.
  - A remote connection shows only what the host reports.
- **Admin:** an "Extensions..." dialog lists `ListEditorExtensions` (state, error, capabilities, hash) with a Reload button for admins.
- **Tests:**
  - Node unit tests: menu building, `enabledWhen`, the invocation flow with fake engine and groups, form validation, event parsing, the trust-store rules.
  - DOM tests ([FE] `src/test-dom/`):
    - the Tools menu is hidden without the capability;
    - items are disabled in read-only and viewer modes (extend `read-only.test.tsx`);
    - the form dialog's validation and submit;
    - an inspector section and its button.
  - Codec vectors come from the regenerated `test-vectors.json`.

---

## 8. Q6: interplay with option B and with runtime scripts

- **A `--play` child:** never loads extensions (the flag combination is refused). Play-time code is the Run context with S4 budgets.
- **The declaration evaluator:** a separate state with different bindings (stubs only), keyed by script source hash, for LuaComponent fields. It shares only the **sandbox builder and the budget hook code** with extension states, never a state.
- **`ExecuteInEditMode`-like behaviour for LuaComponent scripts:** **out of scope, and deliberately not offered.** Under option B, scene scripts never run on the edit host. Re-adding a per-component "run in edit mode" flag would undo B (and its decision 4).
  - The needs it serves get extension-side answers.
  - **Edit-time visuals / gizmos:** later, as **declarative draw lists**. An extension registers `Editor.Gizmos.Register(componentType, fn)`. `fn` returns lines, circles and labels in world space from the component's *data* (fields and transform) when the selection changes. The client draws them like `viewport-gizmo.ts` already does. That needs the `scene.read` capability only, runs on selection change and not per frame, and is budgeted. Interactive handles (drag, then `SetFields` inside a group) come after that.
  - **Validation:** `OnBeforePlay`/`OnSceneSaved` hooks and a "Validate scene" menu item.
  - **Generated content** (procedural placement): menu items with `scene.write`.

---

## 9. Q7: staged plan

Prerequisites:
- **L1** (option B's throwaway-state evaluator; it introduces the shared sandbox builder);
- **L3/S4** (the budget hook and allocator);
- ideally **L2**, so the edit host's global state is already declaration-only when extensions land;
- audit item 7 (fail-closed bind) for the flag-combination rule.

| PR | Repo | Content | Verified by |
|---|---|---|---|
| **E-X1** | engine | `LuaSandbox::Create(profile)` profiles `Declarations` and `EditorExtension` (shared with L1): libraries, no `load`, trimmed `string`, budgets (instructions, wall clock, memory), a "globals of the state" test; pure-Lua math prelude | CI: engine unit tests (`while true`, `string.rep`, absent `io`/`os`/`load`, the prelude) |
| **E-X2** | editor-server | `ExtensionHost`: discovery in `<project>/editor/`, per-file states, `Editor.Extension{}` validation, the registry, the fault flag, `apiVersion`; `--editor-extensions` (+ refusals with `--play` and an open bind without tokens); `ListEditorExtensions`, `GetEditorUi`, `ReloadEditorExtensions`; `extensionsChanged`; capability `"editorExtensions"`; protocol minor bump, generators, vectors | CI: tests with temp projects (a broken file does not stop the others; reload; not loaded without the flag; `--play` refuses; read classes run no Lua: a counting hook asserts 0 instructions) |
| **E-X3** | editor-server | `DispatchNested` (saves `_response`, deny list, `via=` audit, nested cap); `Editor.Scene` read/write, `Editor.Log`, selection in, `select` out; `InvokeMenuItem` with one edit group, rollback on failure, form validation with `ValidateFieldValue`/`ClampFieldValue`; `extensionOutput`; error codes | CI: one invocation = one undo step; undo restores; a failure rolls back with no history step; a viewer is refused (`forbidden`); a read-only host is refused (`readOnly`); a budget trip leaves the host answering and the group closed; a nested write gets the invoker's role; the audit lines appear |
| **E-X4** | frontend | protocol sync; store signals; Tools menu; `FormDialog`; `extension-actions.ts`; console tagging; trust prompt and launcher flag; admin Extensions dialog | CI: frontend unit and DOM tests. **User:** the real Electron app with an example extension: trust prompt, menu, form, undo, console output, a viewer token, play mode disables it |
| **E-X5** | both | Inspector sections and `InvokeInspectorAction` | CI plus **user** (the inspector look) |
| **E-X6** | editor-server (+FE) | Hooks `OnSceneSaved`, `OnSceneOpened`, `OnBeforePlay` (warnings), `OnAssetsChanged`; `Editor.Assets` with `assets.read`/`assets.write`; the `editor/` folder watcher (`--editor-extensions-watch`) | CI; **user** for the warning display |
| **E-X7** | engine docs | `docs/editor-extensions.html` (model, trust, roles, budgets, API), `editor-api.lua` (LuaLS annotations, separate from `engine-api.lua`), an example `lua_project/editor/AlignTools.lua`; update the `docs/editor.html` role text and `docs/scripting-lua.html#sandbox` | CI docs check if one exists [M]; user read-through |

**Minimal useful first slice: E-X1 to E-X4.** That gives "Tools > Align > Snap selection to grid" with a form, one undo step, a console message and the full gating.

**Deferred** (each needs its own small design when wanted):
- gizmo draw lists, then interactive handles;
- custom editor **windows/panels**: a declarative layout of fields, buttons and lists in a dockable panel, refreshed on events;
- **asset importers and post-processors**: hooks inside `assetimport/` would run on every import and on remote hosts. That is a larger trust and perf question, and it touches the import cache;
- long-running jobs with **progress**: they need coroutine slicing across main-loop iterations, plus progress events and cancel;
- dynamic forms (`GetMenuItemForm`);
- an after-play hook;
- per-extension persistent settings;
- writing `editor/` files through the protocol (an admin-only `WriteEditorExtension`);
- macOS native-menu mirroring;
- frontend-side code of any kind (not planned).

---

## 10. Risks

- **The main thread is blocked during an invocation.** It is bounded by the wall-clock budget (5 s), but a heavy extension makes the viewport stall for the invoker. That is acceptable for v1. Jobs with slicing are deferred.
- **Re-entrancy in `EditorServer`.** Handlers assume one command at a time. `DispatchNested` must save and restore `_response`, must not flush the autosave mid-invocation, and must refuse any command that changes the open scene or the history structure. Mitigation: the explicit deny list, plus tests that call every allowed command nested.
- **Atomic rollback can fail** if an op's undo fails: `EditHistory::Undo` then clears the history, as today [V: `EditHistory.hpp:148-150`]. The client gets `full` `sceneChanged`, the same failure mode as a failed user undo.
- **Trust UX.** Folder-level trust means a later `git pull` that adds a malicious extension runs on the next start without a new prompt (the VS Code model). Hash-level trust (D5) is safer but prompts after every edit of your own extension.
- **A coarse capability model.** `scene.write` is all-or-nothing, but it is always capped by the invoker's role.
- **C-level CPU** in allowed library functions is not seen by the count hook. Mitigated by omitting string patterns and by the wall-clock check in the hook, which only fires between VM instructions [M]: `table.sort` with a Lua comparator is counted, and `string.rep` is capped by the allocator.
- **API surface growth** adds maintenance. `apiVersion` plus the plain-table values (no usertypes) keep it decoupled from engine internals.
- **Information exposure.** Extension names and capabilities are visible to viewers (Read). Errors are path-stripped (F10). Output is tagged for item 10 (F9).
- **The protocol version collides** with Part 2's S-4 (1.13.0). That is coordination only.

---

## 11. Decisions for the user

1. **D1 Architecture:** hybrid (host Lua plus declarative UI, no project code in the app) **(recommended)**; host-only; frontend JS; or frontend Lua.
2. **D2 Location:** `<project>/editor/**/*.lua`, outside `assets/` **(recommended:** editors cannot write it, and build exclusion comes by construction**)**; or Unity-style `assets/Editor/**` with a new admin-only write rule and require/play exclusions.
3. **D3 Flag name and default:** `--editor-extensions`, default OFF on every host, refused with `--play` and with an open bind that has no tokens file **(recommended)**; or `--allow-editor-scripts`; or default ON for loopback without a prompt.
4. **D4 Who may invoke:** editor and admin, with an optional per-item `minRole = "admin"` **(recommended)**; or admin only (a host-wide `--editor-extensions-role admin`). Viewers never.
5. **D5 Local trust:** a per-project-folder trust prompt in Electron that gates the flag **(recommended)**; or per content hash (re-prompt on any change); or no prompt.
6. **D6 String patterns** in extension states: omitted in v1 **(recommended)**; or allowed with input-size caps.
7. **D7 Budgets:** load 10M instructions / 1 s, invocation 200M / 5 s, hook 20M / 0.5 s, 64 MiB per state, 10,000 nested commands **(recommended starting values)**; or other values.
8. **D8 A failed invocation:** atomic, rolled back **(recommended)**; or keep partial edits as an undo step.
9. **D9 An open client edit group:** refuse `InvokeMenuItem` while one is open, with the client closing its groups first **(recommended)**; or nest the extension's step into it.
10. **D10 `OnBeforePlay`:** warnings only **(recommended)**; or may block play.
11. **D11 First slice scope:** E-X1 to E-X4 (menu commands over the selection, form, undo, output, gating) **(recommended)**; inspector sections (E-X5) and hooks (E-X6) next; gizmos, panels, importers, jobs and dynamic forms deferred.
12. **D12 Sequencing:** start after L1 and L3 (and ideally L2) so one sandbox builder and one budget hook serve both B and extensions **(recommended)**; or start E-X2 in parallel with a temporary sandbox.
