Lua editor extensions, PR E-X3 of the design (proposals/lua-editor-extensions.md, sections 4.2, 4.3, 6.4, 7.1): `Editor.Scene` read/write and `Editor.Log` in the extension sandbox, every write through a nested dispatch of the real command layer, and `InvokeMenuItem` (0x82, protocol 1.15.0) with one edit group per invocation and atomic rollback.

## What changes

- **Protocol 1.15.0 (additive minor):** `InvokeMenuItem` 0x82 (Write, edit-only: a play host refuses it), response `ExtensionResult` 0x1F (`label, revision, canUndo, canRedo, savedRevision, result: {message?, select?, warnings?}`), the `extensionOutput` event (`extension, level, message, time`), error codes `staleUiRevision` and `editGroupOpen` (new), `extensionFailed` and `extensionFaulted` now answered. 0x83 stays reserved. Clients and golden vectors regenerated (the frontend sync is a separate item).
- **ExtensionHost::Invoke:** the item's `run(ctx, params)` runs on a worker under the invocation budget (200M instructions / 5 s, D7). The worker never touches the editor: each `Editor.Scene`/`Editor.Log` call is a request the command thread answers, so every scene change is made on the command thread. A silent worker is abandoned after its wall budget plus 1 s (the state goes with the job, the extension is faulted). Nested cap 10,000 per invocation, 64 MiB of request/answer bytes, 1000 log lines.
- **Editor namespace:** `Editor.Log.Info/Warn/Error` (and `print`) for every state; `Editor.Scene` installed only for the declared capabilities (scene.read: `GetHierarchy, GetEntity, GetComponent, GetLocalTransform, FindByName, GetComponentTypes`; scene.write: `CreateEntity, Destroy, Duplicate, SetParent, SetProperties, SetLocalTransform, AddComponent, RemoveComponent, SetFields`), absent otherwise. Values are plain tables and UUID strings.
- **EditorServer (EditorServerExtensions.cpp, minimal hooks in EditorServer.cpp):** `DispatchNested` saves and restores `_response`, never flushes the autosave mid-invocation, goes through `ProcessCommand` with the invoker's identity (read-only gate, role gate, audit line with ` via=<extension>`), and refuses every command not in the policy table. `HandleInvokeMenuItem`: the gates (extensionsDisabled, staleUiRevision, unknown item, minRole, extensionFaulted, editGroupOpen; forbidden/readOnly/play from the existing gates), selection (<= 4096 UUIDs) and params checked with `ValidateFieldValue`/`ClampFieldValue` before any Lua, one group labelled with the item's title, `EditHistory::AbortGroup` on any failure (no history step, redo steps kept, the group closed on every path), extensionFailed with a path-stripped message, the detail as an Admin log line, an audit line `extension=<name>@<sha8> item=<id>`.
- **Tests:** `tests/editor-server/ExtensionInvokeTests.cpp` (gates, roles with a tokens file, the policy table against protocol.json and every denied id, ungranted capabilities absent, Editor.Log events and their Admin tag, the nested cap, a budget trip, a stalled worker abandoned within the deadline, three failures fault, path-stripped errors) and `tests/editor-server/project/EditorExtensionInvokeTests.cpp` (one invocation = one undo step, undo/redo, every Editor.Scene function in one step, rollback on a Lua error and a budget trip with no step and the redo kept, a caught refusal keeps its edits, writes that restore make no step, the golden Tools > Align > Snap selection to grid end to end); the play-host refusal in `EditorExtensionPlayTests.cpp`.
- **Docs:** editor.html (invoking a menu item), logging-and-editor.html (1.15.0, the command, codes, event, classification), scripting-lua.html (the Editor.Scene/Editor.Log reference as implemented), testing.html.

## Deviations (judgement calls)

- API names follow design 4.2 (`GetLocalTransform/SetLocalTransform`, `FindByName`, `SetProperties`, `Destroy`, `Duplicate`, `SetFields`), not the brief's `Find/Children/SetTransform/SetName`; `Children` is not offered (`GetHierarchy` has every node's parentId).
- Component calls take the entity id first (`GetComponent(entityId, componentId)`, `RemoveComponent(entityId, componentId)`, `SetFields(entityId, componentId, values)`), because the commands address a component through its entity; design 4.2 wrote `GetComponent(componentId)`.
- The nested dispatch uses a full policy table (every command of protocol.json decided, a test enforces it) whose effect is an ALLOW list of the 13 commands Editor.Scene needs; anything else, an unlisted id included, is refused. The brief asked for a deny list; the table contains the design's deny list and more.
- `extensionOutput` events and their log lines are Admin-tagged (only an admin, or a host without authentication, sees them), per MCP0a/F9.
- A refused nested command raises a Lua error; a script that catches it with pcall and returns normally keeps the changes it made before that call (one step). A call that sends several commands (CreateEntity with components, AddComponent with values) is all or nothing: the group is marked when the call starts and what was recorded after the mark is taken back (`EditHistory::MarkGroup`/`TakeBackGroupTo`) before the error reaches the script; if that cannot be done exactly, the whole invocation fails with extensionFailed.
- Two new error codes, `staleUiRevision` and `editGroupOpen`, for the two refusals the design names but gives no code.
- A failure's message is path-stripped for every client (the extension host strips `<project>` paths always); the same stripped text is the Admin log line.
- `ctx` holds `itemId` and `selection` only.
- The invocation runs on a worker that hands every `Editor.Scene`/`Editor.Log` call to the command thread (the design blocked the command thread for the wall budget); a silent worker is abandoned after the wall budget plus 1 s and faults the extension.
- Rollback uses a new `EditHistory::AbortGroup` (the open group's ops are undone before any step exists) instead of ending the group and undoing the step, so the redo stack is kept.

## Not verified

- Built and run only by CI (nothing builds locally); CI on the latest head not yet seen.
- The unrecoverable take-back path (an op merging into an earlier one, or an undo failing mid take-back; later writes are then refused) has no test: no current op key or hook can trigger it.
- A real stall in C (the tests stand for it with a worker held by the invoke hook).
- The real Electron client (the frontend sync to 1.15.0 is a separate item).


## Review fixes

- Multi-command Editor.Scene calls are all or nothing (tests: a caught CreateEntity with an unknown component and a caught AddComponent with a refused value leave nothing behind).
- A failure-path test with edits undone from scene snapshots (Duplicate, RemoveComponent, Destroy).
- The JSON push into Lua is flattened first, so no object with a destructor is alive across a Lua error.
- The prelude's registry references and the job slot are made under a protected call.
- Docs: via= audit lines exist only on hosts with a tokens file; log lines past the limit are dropped, not errors.

## Re-review fixes

- An item whose only writes were taken back (empty group, moved revision) is handled like NoChange: a saved scene stays saved, historyChanged and sceneChanged follow (test on a saved scene).
- Once a take-back cannot be done exactly, every later write of the call is refused, so nothing outlives the extensionFailed answer.
- The `__n2_host` global is set under a protected call.

## Approved deviations
Accepted by the user 2026-10-13:
- Editor.Scene API names follow design 4.2 (GetLocalTransform/SetLocalTransform, FindByName, SetProperties, Destroy, Duplicate, SetFields), not the brief's Find/Children/SetTransform/SetName; no Children (GetHierarchy has parentId)
- Component calls take the entity id first (GetComponent/RemoveComponent/SetFields(entityId, componentId, ...)) because the commands address a component through its entity
- Nested dispatch uses a full policy table of every protocol command (test-enforced) with an ALLOW effect for the 13 Editor.Scene commands, instead of a pure deny list; it contains the design's deny list
- extensionOutput events and their log lines are Admin-tagged (only admins or unauthenticated hosts see them)
- A refused nested command raises a Lua error; a script that catches it with pcall keeps the edits made before that call; a multi-command call (CreateEntity with components, AddComponent with values) is all or nothing (EditHistory::MarkGroup/TakeBackGroupTo), and if it cannot be taken back exactly the whole invocation fails
- New error codes staleUiRevision and editGroupOpen for the two refusals the design names without a code
- Invocation runs on a worker that hands every Editor.Scene/Editor.Log call to the command thread (design said command thread blocked by the wall budget); a silent worker is abandoned after wall budget + 1 s and faults the extension
- Rollback uses a new EditHistory::AbortGroup (undo the open group's ops before any step exists) instead of EndGroup then undo, so the redo stack is kept
- ctx holds itemId and selection only
- A call whose only writes were taken back leaves an empty group with a moved revision: treated like NoChange (NoteGroupWithoutChange), so a saved scene stays saved
