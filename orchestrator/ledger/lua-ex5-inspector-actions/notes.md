Accepted by the user 2026-10-13:
- Editor.Inspector.Register is callable only while the extension file loads; a call from a running item is a Lua error
- Limits chosen: at most 32 inspector sections per extension and 16 buttons per section
- Inspector button ids must be unique across the host in a namespace separate from menu item ids
- Declaring inspector sections requires the scene.read permission
- Inspector section fields reuse the params shape without default (built to FieldSchema with the same kind rules)
- HandleInvokeMenuItem split into PassExtensionGates/PassExtensionCallGates/RunExtensionCall, shared by 0x82 and 0x83; ExtensionHost Invoke/InvokeAction share InvokeWith
- Component type match uses Component::GetTypeName against the section componentType; missing or mismatched component is an Error before any Lua runs
- docs/testing.html also updated (test file list)
